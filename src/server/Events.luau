--!strict
--[[
	Events
	------
	Rotating, server-wide live events (Donation Central style). Because a script
	can't change real pass prices or mint Robux, these are hype + social
	mechanics: they spotlight players, run donation mini-competitions during a
	window, and crown a winner. Everything is announced to all clients via the
	GameEvent remote, which drives the on-screen banner + countdown.
]]

local Players = game:GetService("Players")
local ReplicatedStorage = game:GetService("ReplicatedStorage")

local shared = ReplicatedStorage:WaitForChild("DonationShared")
local Config = require(shared:WaitForChild("Config"))
local Net = require(shared:WaitForChild("Net"))

local Data = require(script.Parent.Data)
local Donations = require(script.Parent.Donations)
local Stands = require(script.Parent.Stands)

local Events = {}

local T = Config.Theme

-- State for the event currently running (nil during intermission).
local active: {
	id: string,
	capture: { [number]: number }, -- buyerId -> Robux donated during window
	targetUserId: number?,
}? = nil

local function accent(name: string): Color3
	return (T :: any)[name] or T.Accent
end

local function broadcast(payload: { [string]: any })
	Net.event("GameEvent"):FireAllClients(payload)
end

local function pickWeighted()
	local total = 0
	for _, def in Config.EventList do
		total += def.weight
	end
	local roll = math.random() * total
	local acc = 0
	for _, def in Config.EventList do
		acc += def.weight
		if roll <= acc then
			return def
		end
	end
	return Config.EventList[1]
end

local function onlinePlayers(): { Player }
	return Players:GetPlayers()
end

local function highestEarnerOnline(): Player?
	local best: Player? = nil
	local bestVal = -1
	for _, player in onlinePlayers() do
		local p = Data.get(player.UserId)
		if p and p.earnedRobux > bestVal then
			bestVal = p.earnedRobux
			best = player
		end
	end
	return best
end

-- Track donations while a competition event is live.
Donations.onDonation(function(buyerId, _sellerId, price)
	if active and (active.id == "rush" or active.id == "generosity") then
		active.capture[buyerId] = (active.capture[buyerId] or 0) + price
	end
end)

local function resultForCompetition(): string
	if not active then
		return "No winner this time."
	end
	local winnerId, winnerVal = nil, 0
	for buyerId, amount in active.capture do
		if amount > winnerVal then
			winnerVal = amount
			winnerId = buyerId
		end
	end
	if not winnerId then
		return "Nobody donated this round - next time!"
	end
	local winner = Players:GetPlayerByUserId(winnerId)
	local name = winner and winner.DisplayName or ("User" .. winnerId)
	if winner then
		Net.event("Notify"):FireClient(winner, {
			kind = "good",
			text = ("🏆 You won the event with %d R$ donated!"):format(winnerVal),
		})
	end
	return ("🏆 %s donated the most (%d R$)!"):format(name, winnerVal)
end

local function runEvent(def)
	local players = onlinePlayers()
	if #players == 0 then
		return
	end

	active = { id = def.id, capture = {}, targetUserId = nil }
	local targetName: string? = nil

	if def.id == "spotlight" then
		local target = highestEarnerOnline() or players[math.random(#players)]
		if target then
			active.targetUserId = target.UserId
			targetName = target.DisplayName
			Stands.setHighlight(target.UserId, true, accent(def.accent))
		end
	elseif def.id == "lucky" then
		local target = players[math.random(#players)]
		active.targetUserId = target.UserId
		targetName = target.DisplayName
		Stands.setHighlight(target.UserId, true, accent(def.accent))
	end

	broadcast({
		phase = "start",
		id = def.id,
		title = def.title,
		blurb = def.blurb,
		duration = def.duration,
		accent = accent(def.accent),
		targetName = targetName,
	})

	task.wait(def.duration)

	local resultText
	if def.id == "rush" or def.id == "generosity" then
		resultText = resultForCompetition()
	elseif targetName then
		resultText = ("Thanks for supporting %s! 💖"):format(targetName)
	else
		resultText = "Thanks for playing along!"
	end

	if active and active.targetUserId then
		Stands.setHighlight(active.targetUserId, false)
	end
	Stands.clearAllHighlights()

	broadcast({
		phase = "end",
		id = def.id,
		title = def.title,
		resultText = resultText,
	})

	active = nil
end

function Events.start()
	if not Config.Events.Enabled then
		return
	end
	task.spawn(function()
		task.wait(Config.Events.StartupDelay)
		while true do
			local def = pickWeighted()
			runEvent(def)
			task.wait(Config.Events.Intermission)
		end
	end)
end

return Events
