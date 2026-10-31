--!strict
--[[
	Net
	---
	Creates (server) and resolves (client) the RemoteEvents / RemoteFunctions
	used by the game, so neither side hard-codes instance paths. The remotes
	are generated at runtime, which keeps the place file script-only.

	Direction key:
	  S->C events : ProfileUpdated, Notify, LeaderboardUpdated
	  C->S event  : RequestDonation
	  functions   : GetProfile, GetStand, RegisterPass, UnregisterPass,
	                GetLeaderboards, GetAdminStats
]]

local ReplicatedStorage = game:GetService("ReplicatedStorage")

local Net = {}

local FOLDER_NAME = "DonationNet"

local EVENTS = {
	"ProfileUpdated",
	"Notify",
	"LeaderboardUpdated",
	"GameEvent",
	"RequestDonation",
}

local FUNCTIONS = {
	"GetProfile",
	"GetStand",
	"RegisterPass",
	"UnregisterPass",
	"GetLeaderboards",
	"GetAdminStats",
}

-- Server: build the remote folder. Call once on startup.
function Net.setup(): Folder
	local folder = Instance.new("Folder")
	folder.Name = FOLDER_NAME

	for _, name in EVENTS do
		local e = Instance.new("RemoteEvent")
		e.Name = name
		e.Parent = folder
	end
	for _, name in FUNCTIONS do
		local f = Instance.new("RemoteFunction")
		f.Name = name
		f.Parent = folder
	end

	folder.Parent = ReplicatedStorage
	return folder
end

local function folder(): Folder
	return ReplicatedStorage:WaitForChild(FOLDER_NAME) :: Folder
end

function Net.event(name: string): RemoteEvent
	return folder():WaitForChild(name) :: RemoteEvent
end

function Net.func(name: string): RemoteFunction
	return folder():WaitForChild(name) :: RemoteFunction
end

return Net
