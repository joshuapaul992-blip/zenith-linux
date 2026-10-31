--!strict
--[[
	Donations
	---------
	The heart of the game. Players link their OWN game passes to their stand;
	other players buy those passes through the UI. Because each pass is owned by
	the seller, Roblox pays the seller directly (minus the marketplace fee) -
	there is no manual payout step. We just verify ownership, prevent stat
	spoofing, and record totals for the leaderboards + dashboard.
]]

local MarketplaceService = game:GetService("MarketplaceService")
local Players = game:GetService("Players")
local ReplicatedStorage = game:GetService("ReplicatedStorage")

local shared = ReplicatedStorage:WaitForChild("DonationShared")
local Config = require(shared:WaitForChild("Config"))
local Net = require(shared:WaitForChild("Net"))

local Data = require(script.Parent.Data)
local Leaderboards = require(script.Parent.Leaderboards)

local Donations = {}

type Result = { ok: boolean, err: string?, pass: Data.Pass? }

-- Running totals since the server started (for the owner dashboard).
Donations.session = { donated = 0, count = 0 }

-- Listeners notified every time a donation is credited (used by the Events
-- system to run mini-competitions). Signature: (buyerId, sellerId, price).
local donationListeners: { (number, number, number) -> () } = {}
function Donations.onDonation(callback: (number, number, number) -> ())
	table.insert(donationListeners, callback)
end

-- Short-lived cache of pass metadata to avoid hammering GetProductInfo.
local infoCache: { [number]: { price: number, name: string, creatorType: string, creatorId: number, onSale: boolean } } = {}

local function fetchInfo(passId: number)
	local ok, info = pcall(function()
		return MarketplaceService:GetProductInfo(passId, Enum.InfoType.GamePass)
	end)
	if not ok or type(info) ~= "table" then
		return nil
	end
	local creator = info.Creator or {}
	local data = {
		price = tonumber(info.PriceInRobux) or 0,
		name = tostring(info.Name or "Game Pass"),
		creatorType = tostring(creator.CreatorType or "User"),
		creatorId = tonumber(creator.CreatorTargetId) or tonumber(creator.Id) or 0,
		onSale = info.IsForSale == true and (tonumber(info.PriceInRobux) or 0) > 0,
	}
	infoCache[passId] = data
	return data
end

local function notify(player: Player?, kind: string, text: string)
	if player and player.Parent then
		Net.event("Notify"):FireClient(player, { kind = kind, text = text })
	end
end

function Donations.pushProfile(player: Player)
	local profile = Data.get(player.UserId)
	if profile then
		Net.event("ProfileUpdated"):FireClient(player, profile)
	end
end

-- Verify the pass is owned by this player (or the allowed group) so nobody can
-- link someone else's pass to steal leaderboard credit. (Robux always goes to
-- the true owner regardless - this only protects the stats.)
local function ownsPass(player: Player, info): boolean
	if info.creatorType == "Group" then
		return Config.AllowedGroupId ~= 0 and info.creatorId == Config.AllowedGroupId
	end
	return info.creatorId == player.UserId
end

function Donations.registerPass(player: Player, rawId: any): Result
	local passId = tonumber(rawId)
	if not passId or passId <= 0 or passId % 1 ~= 0 then
		return { ok = false, err = "Enter a valid game pass id." }
	end

	local profile = Data.get(player.UserId)
	if not profile then
		return { ok = false, err = "Your data is still loading - try again." }
	end

	if #profile.linkedPasses >= Config.MaxPassesPerPlayer then
		return { ok = false, err = ("You can link at most %d passes."):format(Config.MaxPassesPerPlayer) }
	end
	for _, p in profile.linkedPasses do
		if p.id == passId then
			return { ok = false, err = "You already linked that pass." }
		end
	end

	local info = fetchInfo(passId)
	if not info then
		return { ok = false, err = "Couldn't find that game pass. Check the id." }
	end
	if not info.onSale then
		return { ok = false, err = "That pass isn't on sale. Enable 'For Sale' and set a price." }
	end
	if not ownsPass(player, info) then
		return { ok = false, err = "You can only link passes YOU created." }
	end
	if info.price < Config.MinPassPrice or info.price > Config.MaxPassPrice then
		return { ok = false, err = ("Price must be between %d and %d R$."):format(Config.MinPassPrice, Config.MaxPassPrice) }
	end

	local pass: Data.Pass = { id = passId, price = info.price, name = info.name }
	table.insert(profile.linkedPasses, pass)
	Data.markDirty(player.UserId)
	Data.setPassOwner(passId, player.UserId)
	Donations.pushProfile(player)
	return { ok = true, pass = pass }
end

function Donations.unregisterPass(player: Player, rawId: any): Result
	local passId = tonumber(rawId)
	local profile = Data.get(player.UserId)
	if not passId or not profile then
		return { ok = false, err = "Invalid request." }
	end
	for i, p in profile.linkedPasses do
		if p.id == passId then
			table.remove(profile.linkedPasses, i)
			Data.markDirty(player.UserId)
			Donations.pushProfile(player)
			return { ok = true }
		end
	end
	return { ok = false, err = "Pass not found." }
end

-- Return a seller's public stand (their linked passes) for the donate screen.
function Donations.getStand(viewerUserId: number, rawSellerId: any)
	local sellerId = tonumber(rawSellerId)
	if not sellerId then
		return nil
	end
	local profile = Data.get(sellerId)
	if not profile then
		return nil
	end
	local seller = Players:GetPlayerByUserId(sellerId)
	return {
		userId = sellerId,
		name = seller and seller.DisplayName or ("User" .. sellerId),
		earnedRobux = profile.earnedRobux,
		receivedCount = profile.receivedCount,
		passes = profile.linkedPasses,
	}
end

-- A buyer asks to donate to a seller via one of the seller's passes.
function Donations.requestDonation(buyer: Player, rawSellerId: any, rawPassId: any): Result
	local sellerId = tonumber(rawSellerId)
	local passId = tonumber(rawPassId)
	if not sellerId or not passId then
		return { ok = false, err = "Invalid request." }
	end
	if sellerId == buyer.UserId then
		return { ok = false, err = "You can't donate to yourself." }
	end

	local sellerProfile = Data.get(sellerId)
	if not sellerProfile then
		return { ok = false, err = "That player's stand isn't available right now." }
	end
	local found = false
	for _, p in sellerProfile.linkedPasses do
		if p.id == passId then
			found = true
			break
		end
	end
	if not found then
		return { ok = false, err = "That pass is no longer on this stand." }
	end

	-- A game pass can only be owned once; tell the buyer nicely.
	local ownsOk, owns = pcall(function()
		return MarketplaceService:UserOwnsGamePassAsync(buyer.UserId, passId)
	end)
	if ownsOk and owns then
		return { ok = false, err = "You already own this pass (already donated with it)." }
	end

	MarketplaceService:PromptGamePassPurchase(buyer, passId)
	return { ok = true }
end

-- Fired by Roblox after any game-pass purchase prompt resolves.
local function onPurchaseFinished(buyer: Player, passId: number, wasPurchased: boolean)
	if not wasPurchased then
		return
	end
	local sellerId = Data.getPassOwner(passId)
	if not sellerId then
		return -- not a pass tracked by this game
	end

	local buyerProfile = Data.get(buyer.UserId)
	if not buyerProfile then
		return
	end

	local key = tostring(passId)
	if buyerProfile.boughtPasses[key] then
		return -- already credited (guards against duplicate events)
	end

	local info = fetchInfo(passId)
	local price = info and info.price or 0
	if price <= 0 then
		return
	end

	buyerProfile.boughtPasses[key] = true
	buyerProfile.donatedRobux += price
	buyerProfile.donationCount += 1
	Data.markDirty(buyer.UserId)

	Donations.session.donated += price
	Donations.session.count += 1

	local sellerTotal = Data.creditSeller(sellerId, price)

	Leaderboards.setDonated(buyer.UserId, buyerProfile.donatedRobux)
	Leaderboards.setEarned(sellerId, sellerTotal)

	local net = math.floor(price * (1 - Config.MarketplaceFee))
	notify(buyer, "good", ("Donated %d R$ - thank you! 💖"):format(price))
	Donations.pushProfile(buyer)

	local seller = Players:GetPlayerByUserId(sellerId)
	if seller then
		notify(seller, "good", ("%s donated %d R$ to you (~%d after fees)!"):format(buyer.DisplayName, price, net))
		Donations.pushProfile(seller)
	end

	for _, listener in donationListeners do
		task.spawn(listener, buyer.UserId, sellerId, price)
	end
end

function Donations.start()
	MarketplaceService.PromptGamePassPurchaseFinished:Connect(onPurchaseFinished)
end

return Donations
