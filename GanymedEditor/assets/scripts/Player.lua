-- Example gameplay script.
--
-- A script is a Lua module returning a table of lifecycle methods. The engine creates one
-- instance table per entity whose metatable's __index points at this table, so methods are
-- shared and `self` is per-entity. `self.entity` is injected before OnCreate runs.
--
-- Note the Get -> mutate the copy -> Set shape below. The setter is not a style choice: it is
-- what routes the write through the engine's change tracking, and without it TransformSystem
-- never refreshes the world-transform cache and the entity does not move on screen.

local Player = {}

function Player:OnCreate()
	self.speed = 3.0
	self.elapsed = 0.0
	Log.Info("Player created: " .. self.entity:GetName())
end

function Player:OnUpdate(ts)
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

	-- Unconditional bob, so the script visibly does something without input.
	pos.y = math.sin(self.elapsed * 2.0)

	self.entity:SetTranslation(pos)
end

function Player:OnCollisionEnter(other)
	Log.Warn("Hit " .. other:GetName())
end

function Player:OnDestroy()
	Log.Info("Player destroyed")
end

return Player
