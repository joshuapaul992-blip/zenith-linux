--!strict
--[[
	DonationCentral - server bootstrap
	----------------------------------
	Wires up networking, persistence, the donation logic, leaderboards, the
	procedural world, live events and the owner dashboard. This Script lives in
	ServerScriptService; its sibling ModuleScripts are the subsystems.
]]

local Players = game:GetService("Players")
local RunService = game:GetService("RunService")

local Net = require(game:GetService("ReplicatedStorage"):WaitForChild("DonationShared"):WaitForChild("Net"))

local Data = require(script.Data)
local Donations = require(script.Donations)
local Leaderboards = require(script.Leaderboards)
local Stands = require(script.Stands)
local Events = require(script.Events)
local Admin = require(script.Admin)

-- 1. Remotes first, so clients can bind immediately.
Net.setup()

-- 2. Build the world.
Stands.buildWorld()

-- 3. Players.
local function onJoin(player: Player)
	Data.load(player)
	Donations.pushProfile(player)
	Stands.createStand(player)
end

local function onLeave(player: Player)
	Stands.removeStand(player)
	-- Push the player's final totals to the leaderboards before releasing.
	local profile = Data.get(player.UserId)
	if profile then
		Leaderboards.setEarned(player.UserId, profile.earnedRobux)
		Leaderboards.setDonated(player.UserId, profile.donatedRobux)
	end
	Data.release(player.UserId)
end

Players.PlayerAdded:Connect(onJoin)
Players.PlayerRemoving:Connect(onLeave)
for _, player in Players:GetPlayers() do
	task.spawn(onJoin, player)
end

-- 4. Remote handlers.
Net.func("GetProfile").OnServerInvoke = function(player)
	return Data.get(player.UserId)
end

Net.func("GetStand").OnServerInvoke = function(player, sellerUserId)
	return Donations.getStand(player.UserId, sellerUserId)
end

Net.func("RegisterPass").OnServerInvoke = function(player, passId)
	return Donations.registerPass(player, passId)
end

Net.func("UnregisterPass").OnServerInvoke = function(player, passId)
	return Donations.unregisterPass(player, passId)
end

Net.func("GetLeaderboards").OnServerInvoke = function()
	return Leaderboards.get()
end

Net.func("GetAdminStats").OnServerInvoke = function(player)
	return Admin.getStats(player)
end

Net.event("RequestDonation").OnServerEvent:Connect(function(player, sellerUserId, passId)
	local result = Donations.requestDonation(player, sellerUserId, passId)
	if not result.ok and result.err then
		Net.event("Notify"):FireClient(player, { kind = "bad", text = result.err })
	end
end)

-- 5. Start subsystems.
Donations.start()
Leaderboards.start()
Stands.start()
Events.start()

-- 6. Autosave loop.
task.spawn(function()
	while true do
		task.wait(60)
		for _, player in Players:GetPlayers() do
			Data.save(player.UserId)
		end
	end
end)

-- 7. Flush everything on shutdown.
game:BindToClose(function()
	if RunService:IsStudio() then
		return
	end
	for _, player in Players:GetPlayers() do
		local profile = Data.get(player.UserId)
		if profile then
			Leaderboards.setEarned(player.UserId, profile.earnedRobux)
			Leaderboards.setDonated(player.UserId, profile.donatedRobux)
		end
		Data.release(player.UserId)
	end
	task.wait(2)
end)

print("[DonationCentral] Server ready.")
