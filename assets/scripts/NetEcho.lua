local Echo = {
	Replicated = {
		health = "u8",
		weapon = "str16",
		attackTick = "tick",
		down = "bool",
	},
}

local sent = false

function Echo:OnTick(tick, cmd)
	if not Net.IsServer() or sent or tick < 1200 then
		return
	end
	sent = true
	self.health = 40
	self.weapon = "rifle"
	self.attackTick = tick
	self.down = true
	Net.Broadcast("ping", { n = 7, note = "hi" })
	Net.Broadcast("dead", { who = "alpha" }, true)
	Net.Shot(self.entity, Vec3(1, 2, 3), Vec3(0, 0, 1), 20)
	Log.Info(string.format("Echo fired tick=%d", tick))
end

function Echo:OnUpdate(ts)
	if not Net.IsClient() or self.reported or self.health ~= 40 then
		return
	end
	self.reported = true
	Log.Info(string.format("Field health=%d weapon=%s attack=%d down=%s",
		self.health, tostring(self.weapon), self.attackTick, tostring(self.down)))
end

function Echo:OnNetEvent(name, args)
	if not Net.IsClient() then
		return
	end
	if name == "ping" then
		Log.Info(string.format("Event ping n=%g note=%s", args.n, tostring(args.note)))
	elseif name == "dead" then
		Log.Info("Event dead who=" .. tostring(args.who))
	else
		Log.Info("Event " .. name)
	end
end

function Echo:OnShot(muzzle, direction, speed, tick)
	if not Net.IsClient() then
		return
	end
	Log.Info(string.format("Shot x=%.3f y=%.3f z=%.3f dz=%.3f speed=%.3f tick=%d",
		muzzle.x, muzzle.y, muzzle.z, direction.z, speed, tick))
end

return Echo
