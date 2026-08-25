--[[ Generated with https://github.com/TypeScriptToLua/TypeScriptToLua ]]
local ____exports = {}
local Player = {
    entity = nil,
    Properties = {speed = 3, drainRate = 12, bobHeight = 1, masterVolume = 0.8},
    speed = 0,
    drainRate = 0,
    bobHeight = 0,
    masterVolume = 0,
    elapsed = 0,
    health = 100,
    score = 0,
    musicMuted = false,
    musicKeyWasDown = false,
    humFrames = 0,
    OnCreate = function(self)
        Log.Info(((("Player created: " .. self.entity:GetName()) .. " (speed=") .. tostring(self.speed)) .. ")")
        UI.SetHealth(self.health)
        UI.SetScore(self.score)
        Audio.SetMasterVolume(self.masterVolume)
        Log.Info("Player has an audio source: " .. tostring(self.entity:HasAudioSource()))
        self.entity:SetSoundLooping(true)
        self.entity:SetSoundVolume(0.6)
    end,
    OnUpdate = function(self, ts)
        self.elapsed = self.elapsed + ts
        self.health = self.health - ts * self.drainRate
        if self.health <= 0 then
            self.health = 100
            Audio.PlayOneShot("audio/chime.wav")
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
        local bob = math.sin(self.elapsed * 2)
        pos.y = bob * self.bobHeight
        self.entity:SetTranslation(pos)
        if Input.IsKeyPressed(Key.Space) then
            self.entity:PlaySound()
            self.entity:SetSoundPitch(1 + bob * 0.15)
            self.humFrames = self.humFrames + 1
            if self.humFrames == 1 or self.humFrames % 120 == 0 then
                Log.Info((("Hum: " .. tostring(self.humFrames)) .. " PlaySound calls, IsSoundPlaying=") .. tostring(self.entity:IsSoundPlaying()))
            end
        elseif self.humFrames > 0 then
            self.entity:StopSound()
            Log.Info((("Hum stopped after " .. tostring(self.humFrames)) .. " PlaySound calls, IsSoundPlaying=") .. tostring(self.entity:IsSoundPlaying()))
            self.humFrames = 0
        end
        local musicKeyDown = Input.IsKeyPressed(Key.M)
        if musicKeyDown and not self.musicKeyWasDown then
            self.musicMuted = not self.musicMuted
            Audio.SetGroupVolume("Music", self.musicMuted and 0 or 1)
            Log.Info("Music " .. (self.musicMuted and "muted" or "unmuted"))
        end
        self.musicKeyWasDown = musicKeyDown
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
