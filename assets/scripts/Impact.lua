--[[ Generated with https://github.com/TypeScriptToLua/TypeScriptToLua ]]
local ____exports = {}
local Impact = {
    entity = nil,
    Properties = {clip = "audio/impact.wav", minInterval = 0.12, sparkCount = 24},
    clip = "",
    minInterval = 0,
    sparkCount = 0,
    cooldown = 0,
    OnUpdate = function(self, ts)
        if self.cooldown > 0 then
            self.cooldown = self.cooldown - ts
        end
    end,
    OnCollisionEnter = function(self, other)
        if self.cooldown > 0 then
            return
        end
        self.cooldown = self.minInterval
        Audio.PlayOneShot(
            self.clip,
            self.entity:GetTranslation()
        )
        Log.Trace((("Impact: " .. self.entity:GetName()) .. " hit ") .. other:GetName())
        local sparks = self.entity:GetChildByName("Sparks")
        if sparks then
            sparks:PlayParticles()
            sparks:EmitBurst(self.sparkCount)
        end
    end
}
____exports.default = Impact
return ____exports
