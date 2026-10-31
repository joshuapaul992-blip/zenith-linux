--!strict
--[[
	Admin
	-----
	Owner-only dashboard data. Because gamepass donations pay each seller
	directly through Roblox, there is nothing to "pay out" by hand - this panel
	is an at-a-glance view of how the game is performing: who's earning, who's
	donating, and the live totals for this server.
]]

local Players = game:GetService("Players")
local ReplicatedStorage = game:GetService("ReplicatedStorage")

local shared = ReplicatedStorage:WaitForChild("DonationShared")
local Config = require(shared:WaitForChild("Config"))

local Data = require(script.Parent.Data)
local Donations = require(script.Parent.Donations)
local Leaderboards = require(script.Parent.Leaderboards)

local Admin = {}

local ownerSet: { [number]: boolean } = {}
for _, id in Config.OwnerUserIds do
	ownerSet[id] = true
end

function Admin.isOwner(player: Player): boolean
	if ownerSet[player.UserId] then
		return true
	end
	-- The experience creator is always an owner, even if not listed in Config.
	if game.CreatorType == Enum.CreatorType.User and player.UserId == game.CreatorId then
		return true
	end
	return false
end

function Admin.getStats(requester: Player)
	if not Admin.isOwner(requester) then
		return { ok = false, err = "Owner only." }
	end

	local online = {}
	for _, player in Players:GetPlayers() do
		local p = Data.get(player.UserId)
		if p then
			table.insert(online, {
				name = player.DisplayName,
				userId = player.UserId,
				earnedRobux = p.earnedRobux,
				donatedRobux = p.donatedRobux,
				linkedPasses = #p.linkedPasses,
			})
		end
	end
	table.sort(online, function(a, b)
		return a.earnedRobux > b.earnedRobux
	end)

	local lb = Leaderboards.get()
	return {
		ok = true,
		feePct = math.floor(Config.MarketplaceFee * 100),
		session = {
			donated = Donations.session.donated,
			count = Donations.session.count,
			players = #Players:GetPlayers(),
		},
		online = online,
		topEarners = lb.topEarners,
		topDonors = lb.topDonors,
	}
end

return Admin
