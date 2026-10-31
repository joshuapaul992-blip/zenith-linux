--!strict
--[[
	Leaderboards
	------------
	Global "Top Earners" and "Top Donors" boards backed by OrderedDataStores.
	Totals are pushed to clients on an interval and re-rendered onto any
	physical board surfaces registered by the Stands module.
]]

local DataStoreService = game:GetService("DataStoreService")
local Players = game:GetService("Players")

local ReplicatedStorage = game:GetService("ReplicatedStorage")
local shared = ReplicatedStorage:WaitForChild("DonationShared")
local Config = require(shared:WaitForChild("Config"))
local Net = require(shared:WaitForChild("Net"))

local Leaderboards = {}

local earnedStore = DataStoreService:GetOrderedDataStore("LB_Earned_v2")
local donatedStore = DataStoreService:GetOrderedDataStore("LB_Donated_v2")

export type Entry = { userId: number, name: string, value: number }
export type Snapshot = { topEarners: { Entry }, topDonors: { Entry } }

local snapshot: Snapshot = { topEarners = {}, topDonors = {} }
local nameCache: { [number]: string } = {}
local refreshSubs: { (Snapshot) -> () } = {}

local function withRetry(tries: number, fn: () -> any): (boolean, any)
	local attempt = 0
	local lastErr
	while attempt < tries do
		attempt += 1
		local ok, result = pcall(fn)
		if ok then
			return true, result
		end
		lastErr = result
		task.wait(2 ^ attempt * 0.25)
	end
	return false, lastErr
end

local function resolveName(userId: number): string
	if nameCache[userId] then
		return nameCache[userId]
	end
	local ok, name = pcall(function()
		return Players:GetNameFromUserIdAsync(userId)
	end)
	local resolved = (ok and name) or ("User" .. userId)
	nameCache[userId] = resolved
	return resolved
end

-- Write an absolute total for a player into an ordered store.
local function setOrdered(store: OrderedDataStore, userId: number, total: number)
	-- OrderedDataStores only accept integers.
	local value = math.max(0, math.floor(total))
	withRetry(4, function()
		store:SetAsync(tostring(userId), value)
	end)
end

function Leaderboards.setEarned(userId: number, total: number)
	setOrdered(earnedStore, userId, total)
end

function Leaderboards.setDonated(userId: number, total: number)
	setOrdered(donatedStore, userId, total)
end

local function readTop(store: OrderedDataStore): { Entry }
	local entries: { Entry } = {}
	local ok, pages = withRetry(4, function()
		return store:GetSortedAsync(false, Config.LeaderboardSize)
	end)
	if not ok or not pages then
		return entries
	end
	local page = pages:GetCurrentPage()
	for _, item in page do
		local userId = tonumber(item.key)
		if userId then
			table.insert(entries, {
				userId = userId,
				name = resolveName(userId),
				value = tonumber(item.value) or 0,
			})
		end
	end
	return entries
end

function Leaderboards.refresh(): Snapshot
	snapshot = {
		topEarners = readTop(earnedStore),
		topDonors = readTop(donatedStore),
	}
	for _, sub in refreshSubs do
		task.spawn(sub, snapshot)
	end
	Net.event("LeaderboardUpdated"):FireAllClients(snapshot)
	return snapshot
end

function Leaderboards.get(): Snapshot
	return snapshot
end

-- Let other systems (e.g. physical boards) redraw when data refreshes.
function Leaderboards.onRefresh(callback: (Snapshot) -> ())
	table.insert(refreshSubs, callback)
	callback(snapshot)
end

function Leaderboards.start()
	task.spawn(function()
		while true do
			Leaderboards.refresh()
			task.wait(Config.LeaderboardRefresh)
		end
	end)
end

return Leaderboards
