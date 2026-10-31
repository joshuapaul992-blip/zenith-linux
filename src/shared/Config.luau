--!strict
--[[
	Config
	------
	Central settings for the donation game. Everything a game owner needs to
	tweak lives here. This module is shared (ReplicatedStorage) so the client
	UI and the server logic agree on the same numbers and colours.

	ALL visuals are built from solid colours + gradients in code, so there are
	NO external image/decal asset IDs anywhere in this project. Nothing here is
	copied from another experience.
]]

local Config = {}

-- Roblox userIds that may open the owner dashboard. Add yours here.
-- Find your id at https://www.roblox.com/users/profile (it's in the URL).
Config.OwnerUserIds = {
	-- 123456789,
} :: { number }

-- If your experience is published under a GROUP, set this to that group id so
-- players can link group-owned game passes. Leave 0 to allow only passes a
-- player created on their own account.
Config.AllowedGroupId = 0

-- Game-pass price guard rails (in Robux).
Config.MinPassPrice = 1
Config.MaxPassPrice = 100000

-- How many passes a single player may link to their stand.
Config.MaxPassesPerPlayer = 12

-- Global leaderboards.
Config.LeaderboardSize = 25 -- entries shown
Config.LeaderboardRefresh = 30 -- seconds between refreshes

-- Roblox marketplace fee. Sellers NET this much less than the sticker price.
-- Used for display only ("you'll receive ~70%").
Config.MarketplaceFee = 0.30

-- -------------------------------------------------------------- in-game events
-- Rotating, server-wide events (like Donation Central's live events). These are
-- social/hype mechanics: Roblox won't let a script change a pass price or mint
-- Robux, so events spotlight players, run mini-competitions, and announce
-- winners rather than altering real payouts.
Config.Events = {
	Enabled = true,
	Intermission = 150, -- seconds of quiet between events
	StartupDelay = 45, -- grace period after the server boots
}

-- Each event: id, title, blurb (shown in the banner), duration (seconds),
-- weight (relative pick chance) and an accent colour name from Theme.
Config.EventList = {
	{
		id = "rush",
		title = "💸 DONATION RUSH",
		blurb = "Every donation in the next window counts toward the Rush crown!",
		duration = 90,
		weight = 3,
		accent = "Accent",
	},
	{
		id = "spotlight",
		title = "🌟 DONOR SPOTLIGHT",
		blurb = "One generous player is in the spotlight - show them love!",
		duration = 60,
		weight = 2,
		accent = "Gold",
	},
	{
		id = "lucky",
		title = "🍀 LUCKY STAND",
		blurb = "A random stand is glowing lucky - donate there for good karma!",
		duration = 60,
		weight = 2,
		accent = "Accent2",
	},
	{
		id = "generosity",
		title = "🎉 GENEROSITY HOUR",
		blurb = "Spread kindness! The most generous donor this window gets a shoutout.",
		duration = 120,
		weight = 2,
		accent = "Warn",
	},
}

-- Original, code-drawn theme. Swap these to re-skin the whole game.
Config.Theme = {
	Background = Color3.fromRGB(16, 18, 27),
	Panel = Color3.fromRGB(27, 30, 44),
	Panel2 = Color3.fromRGB(37, 41, 59),
	Stroke = Color3.fromRGB(58, 64, 90),
	Accent = Color3.fromRGB(112, 138, 255),
	Accent2 = Color3.fromRGB(86, 226, 176),
	Good = Color3.fromRGB(86, 226, 176),
	Warn = Color3.fromRGB(255, 190, 96),
	Bad = Color3.fromRGB(255, 108, 124),
	Text = Color3.fromRGB(237, 240, 249),
	SubText = Color3.fromRGB(150, 158, 182),
	Gold = Color3.fromRGB(255, 209, 102),
	Silver = Color3.fromRGB(205, 214, 230),
	Bronze = Color3.fromRGB(214, 149, 102),
}

return Config
