local Walker = {
	Properties = {
		speed = 4,
	},
}

function Walker:OnTick(tick, cmd)
	local vel = self.entity:GetLinearVelocity()
	local y = vel.y
	if cmd.Jump and self.entity:IsGrounded() then
		y = 4.5
	end
	self.entity:SetLinearVelocity(Vec3(cmd.MoveX * self.speed, y, cmd.MoveZ * self.speed))
end

return Walker
