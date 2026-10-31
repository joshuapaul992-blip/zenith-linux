--!strict
--[[
	Data
	----
	Per-player persistence built on DataStoreService, plus a small global store
	mapping a game-pass id -> the userId that linked it (so we can credit a
	seller even while they are offline).

	Design notes:
	  * In-memory cache per online player, flushed on autosave / leave / close.
	  * Retries with backoff around every DataStore call.
	  * Offline sellers are credited with UpdateAsync (atomic read-modify-write).
	  * No external dependencies - pure DataStoreService.
]]

local DataStoreService = game:GetService("DataStoreService")
local Players = game:GetService("Players")

local Data = {}

local PROFILE_STORE = "DonationProfiles_v2"
local OWNER_STORE = "PassOwners_v2"

local profileStore = DataStoreService:GetDataStore(PROFILE_STORE)
local ownerStore = DataStoreService:GetDataStore(OWNER_STORE)

export type Pass = {
	id: number,
	price: number,
	name: string,
}

export type Profile = {
	linkedPasses: { Pass },
	donatedRobux: number, -- gross Robux this player has donated to others
	earnedRobux: number, -- gross Robux others have donated to this player
	donationCount: number, -- how many donations this player made
	receivedCount: number, -- how many donations this player received
	boughtPasses: { [string]: boolean }, -- dedupe guard, keyed by passId
}

local function defaultProfile(): Profile
	return {
		linkedPasses = {},
		donatedRobux = 0,
		earnedRobux = 0,
		donationCount = 0,
		receivedCount = 0,
		boughtPasses = {},
	}
end

-- userId -> Profile (only for online players)
local cache: { [number]: Profile } = {}
local dirty: { [number]: boolean } = {}

-- Retry helper: runs fn() up to `tries` times with exponential backoff.
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
		task.wait(2 ^ attempt * 0.25) -- 0.5s, 1s, 2s, ...
	end
	return false, lastErr
end

-- Normalise whatever came back from the store into a complete Profile.
local function sanitize(raw: any): Profile
	local p = defaultProfile()
	if type(raw) == "table" then
		if type(raw.linkedPasses) == "table" then
			for _, entry in raw.linkedPasses do
				if type(entry) == "table" and type(entry.id) == "number" then
					table.insert(p.linkedPasses, {
						id = entry.id,
						price = tonumber(entry.price) or 0,
						name = tostring(entry.name or "Game Pass"),
					})
				end
			end
		end
		p.donatedRobux = tonumber(raw.donatedRobux) or 0
		p.earnedRobux = tonumber(raw.earnedRobux) or 0
		p.donationCount = tonumber(raw.donationCount) or 0
		p.receivedCount = tonumber(raw.receivedCount) or 0
		if type(raw.boughtPasses) == "table" then
			for k, v in raw.boughtPasses do
				if v then
					p.boughtPasses[tostring(k)] = true
				end
			end
		end
	end
	return p
end

function Data.load(player: Player): Profile
	local key = "u_" .. player.UserId
	local ok, raw = withRetry(5, function()
		return profileStore:GetAsync(key)
	end)
	if not ok then
		warn(("[Data] load failed for %d: %s"):format(player.UserId, tostring(raw)))
	end
	local profile = sanitize(ok and raw or nil)
	cache[player.UserId] = profile
	return profile
end

function Data.get(userId: number): Profile?
	return cache[userId]
end

function Data.markDirty(userId: number)
	dirty[userId] = true
end

local function writeProfile(userId: number, profile: Profile): boolean
	local key = "u_" .. userId
	local ok, err = withRetry(5, function()
		profileStore:SetAsync(key, profile, { userId })
	end)
	if not ok then
		warn(("[Data] save failed for %d: %s"):format(userId, tostring(err)))
	end
	return ok
end

function Data.save(userId: number)
	local profile = cache[userId]
	if profile and dirty[userId] then
		if writeProfile(userId, profile) then
			dirty[userId] = nil
		end
	end
end

function Data.release(userId: number)
	Data.save(userId)
	cache[userId] = nil
	dirty[userId] = nil
end

-- Credit a SELLER who just received a donation. Works whether or not they are
-- currently in the server. Returns the seller's new gross earned total so the
-- caller can update the leaderboard.
function Data.creditSeller(userId: number, grossPrice: number): number
	local profile = cache[userId]
	if profile then
		profile.earnedRobux += grossPrice
		profile.receivedCount += 1
		Data.markDirty(userId)
		return profile.earnedRobux
	end

	-- Offline: atomic update so we never lose a concurrent donation.
	local key = "u_" .. userId
	local newTotal = grossPrice
	withRetry(5, function()
		profileStore:UpdateAsync(key, function(raw)
			local p = sanitize(raw)
			p.earnedRobux += grossPrice
			p.receivedCount += 1
			newTotal = p.earnedRobux
			return p
		end)
	end)
	return newTotal
end

-- Record / look up which player linked a given pass id.
function Data.setPassOwner(passId: number, userId: number)
	withRetry(5, function()
		ownerStore:SetAsync("p_" .. passId, userId)
	end)
end

function Data.getPassOwner(passId: number): number?
	local ok, result = withRetry(5, function()
		return ownerStore:GetAsync("p_" .. passId)
	end)
	if ok and type(result) == "number" then
		return result
	end
	return nil
end

return Data
