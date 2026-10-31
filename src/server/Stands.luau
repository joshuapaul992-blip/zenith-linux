--!strict
--[[
	Stands
	------
	Builds the physical world entirely from code (no imported models): a floor,
	a spawn, two glowing leaderboard pillars, and a personal podium for each
	player arranged in a ring. Walking up to a podium and pressing the prompt
	opens that player's donation stand on the client.
]]

local Players = game:GetService("Players")
local Workspace = game:GetService("Workspace")
local Lighting = game:GetService("Lighting")
local ReplicatedStorage = game:GetService("ReplicatedStorage")

local shared = ReplicatedStorage:WaitForChild("DonationShared")
local Config = require(shared:WaitForChild("Config"))

local Data = require(script.Parent.Data)
local Leaderboards = require(script.Parent.Leaderboards)

local Stands = {}

local T = Config.Theme
local RING_RADIUS = 32
local MAX_SLOTS = 24

local worldFolder: Folder
local stands: { [number]: Model } = {}
local slotTaken: { [number]: boolean } = {}

local function makePart(props: { [string]: any }): Part
	local p = Instance.new("Part")
	p.Anchored = true
	p.Material = Enum.Material.SmoothPlastic
	for k, v in props do
		(p :: any)[k] = v
	end
	return p
end

local function roundify(gui: Instance, radius: number)
	local c = Instance.new("UICorner")
	c.CornerRadius = UDim.new(0, radius)
	c.Parent = gui
end

-- ---------------------------------------------------------------- world base
function Stands.buildWorld()
	worldFolder = Instance.new("Folder")
	worldFolder.Name = "DonationWorld"

	local floor = makePart({
		Name = "Floor",
		Size = Vector3.new(220, 2, 220),
		Position = Vector3.new(0, -1, 0),
		Color = Color3.fromRGB(32, 35, 50),
		Material = Enum.Material.Concrete,
	})
	floor.Parent = worldFolder

	-- Decorative accent ring on the floor.
	local ring = makePart({
		Name = "AccentRing",
		Shape = Enum.PartType.Cylinder,
		Size = Vector3.new(0.4, RING_RADIUS * 2 + 10, RING_RADIUS * 2 + 10),
		CFrame = CFrame.new(0, 0.1, 0) * CFrame.Angles(0, 0, math.rad(90)),
		Color = T.Accent,
		Material = Enum.Material.Neon,
		Transparency = 0.4,
	})
	ring.Parent = worldFolder

	local spawn = Instance.new("SpawnLocation")
	spawn.Name = "Spawn"
	spawn.Anchored = true
	spawn.Size = Vector3.new(10, 1, 10)
	spawn.Position = Vector3.new(0, 0.5, 0)
	spawn.Color = T.Accent2
	spawn.Material = Enum.Material.Neon
	spawn.Neutral = true
	spawn.Duration = 0
	spawn.Parent = worldFolder

	-- A welcome sign over spawn.
	local pole = makePart({
		Name = "SignPole",
		Size = Vector3.new(1, 14, 1),
		Position = Vector3.new(0, 7, 0),
		Color = Color3.fromRGB(45, 49, 68),
	})
	pole.Parent = worldFolder
	local board = makePart({
		Name = "WelcomeBoard",
		Size = Vector3.new(24, 7, 1),
		Position = Vector3.new(0, 15, 0),
		Color = T.Panel,
	})
	board.Parent = worldFolder
	local sg = Instance.new("SurfaceGui")
	sg.Face = Enum.NormalId.Front
	sg.CanvasSize = Vector2.new(480, 140)
	sg.Parent = board
	local title = Instance.new("TextLabel")
	title.Size = UDim2.fromScale(1, 0.6)
	title.BackgroundTransparency = 1
	title.Font = Enum.Font.GothamBlack
	title.Text = "DONATION PLAZA"
	title.TextColor3 = T.Text
	title.TextScaled = true
	title.Parent = sg
	local sub = Instance.new("TextLabel")
	sub.Position = UDim2.fromScale(0, 0.6)
	sub.Size = UDim2.fromScale(1, 0.4)
	sub.BackgroundTransparency = 1
	sub.Font = Enum.Font.GothamMedium
	sub.Text = "Walk to a stand to donate • Link your passes to earn"
	sub.TextColor3 = T.SubText
	sub.TextScaled = true
	sub.Parent = sg

	Stands.buildLeaderboardPillars(worldFolder)

	worldFolder.Parent = Workspace

	-- Pleasant lighting.
	Lighting.ClockTime = 15
	Lighting.Brightness = 2
	Lighting.Ambient = Color3.fromRGB(70, 72, 90)
	Lighting.OutdoorAmbient = Color3.fromRGB(90, 94, 120)
end

-- --------------------------------------------------------- leaderboard boards
local function renderBoard(surface: SurfaceGui, heading: string, accent: Color3, entries)
	for _, child in surface:GetChildren() do
		child:Destroy()
	end

	local header = Instance.new("TextLabel")
	header.Size = UDim2.new(1, -16, 0, 48)
	header.Position = UDim2.new(0, 8, 0, 8)
	header.BackgroundColor3 = accent
	header.Font = Enum.Font.GothamBlack
	header.Text = heading
	header.TextColor3 = Color3.fromRGB(20, 22, 32)
	header.TextScaled = true
	header.Parent = surface
	roundify(header, 8)

	local list = Instance.new("Frame")
	list.Size = UDim2.new(1, -16, 1, -68)
	list.Position = UDim2.new(0, 8, 0, 64)
	list.BackgroundTransparency = 1
	list.Parent = surface
	local layout = Instance.new("UIListLayout")
	layout.Padding = UDim.new(0, 4)
	layout.Parent = list

	if #entries == 0 then
		local empty = Instance.new("TextLabel")
		empty.Size = UDim2.new(1, 0, 0, 40)
		empty.BackgroundTransparency = 1
		empty.Font = Enum.Font.GothamMedium
		empty.Text = "No donations yet - be the first!"
		empty.TextColor3 = T.SubText
		empty.TextScaled = true
		empty.Parent = list
		return
	end

	for rank, entry in entries do
		if rank > 10 then
			break
		end
		local row = Instance.new("TextLabel")
		row.Size = UDim2.new(1, 0, 0, 34)
		row.BackgroundColor3 = T.Panel2
		row.BackgroundTransparency = 0.2
		row.Font = Enum.Font.GothamMedium
		row.TextColor3 = T.Text
		row.TextScaled = true
		row.TextXAlignment = Enum.TextXAlignment.Left
		row.Text = ("  #%d  %s"):format(rank, entry.name)
		row.Parent = list
		roundify(row, 6)

		local amount = Instance.new("TextLabel")
		amount.Size = UDim2.new(0.4, 0, 1, 0)
		amount.Position = UDim2.new(0.6, 0, 0, 0)
		amount.BackgroundTransparency = 1
		amount.Font = Enum.Font.GothamBold
		amount.TextColor3 = accent
		amount.TextScaled = true
		amount.TextXAlignment = Enum.TextXAlignment.Right
		amount.Text = ("%s R$  "):format(Stands.comma(entry.value))
		amount.Parent = row
	end
end

function Stands.comma(n: number): string
	local s = tostring(math.floor(n))
	local formatted = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
	return (formatted:gsub("^,", ""))
end

function Stands.buildLeaderboardPillars(parent: Instance)
	local defs = {
		{ heading = "TOP EARNERS", accent = T.Accent2, pos = Vector3.new(-14, 9, -24), key = "topEarners" },
		{ heading = "TOP DONORS", accent = T.Accent, pos = Vector3.new(14, 9, -24), key = "topDonors" },
	}
	local surfaces = {}
	for _, def in defs do
		local pillar = makePart({
			Name = def.heading,
			Size = Vector3.new(16, 18, 1),
			Position = def.pos,
			Color = T.Panel,
		})
		pillar.Parent = parent
		local sg = Instance.new("SurfaceGui")
		sg.Face = Enum.NormalId.Front
		sg.CanvasSize = Vector2.new(360, 420)
		sg.Parent = pillar
		surfaces[def.key] = { gui = sg, heading = def.heading, accent = def.accent }
		renderBoard(sg, def.heading, def.accent, {})
	end

	Leaderboards.onRefresh(function(snapshot)
		for key, info in surfaces do
			renderBoard(info.gui, info.heading, info.accent, (snapshot :: any)[key] or {})
		end
	end)
end

-- --------------------------------------------------------------- player stands
local function freeSlot(): number?
	for i = 0, MAX_SLOTS - 1 do
		if not slotTaken[i] then
			slotTaken[i] = true
			return i
		end
	end
	return nil
end

local function slotCFrame(slot: number): CFrame
	local angle = (slot / MAX_SLOTS) * math.pi * 2
	local pos = Vector3.new(math.cos(angle) * RING_RADIUS, 1.5, math.sin(angle) * RING_RADIUS)
	return CFrame.lookAt(pos, Vector3.new(0, 1.5, 0))
end

function Stands.createStand(player: Player)
	local slot = freeSlot()
	if not slot then
		return
	end

	local model = Instance.new("Model")
	model.Name = "Stand_" .. player.UserId
	model:SetAttribute("OwnerUserId", player.UserId)
	model:SetAttribute("Slot", slot)

	local cframe = slotCFrame(slot)

	local base = makePart({
		Name = "Base",
		Size = Vector3.new(6, 3, 6),
		CFrame = cframe,
		Color = T.Panel,
	})
	base.Parent = model

	local top = makePart({
		Name = "Top",
		Size = Vector3.new(6, 0.4, 6),
		CFrame = cframe * CFrame.new(0, 1.7, 0),
		Color = player.UserId % 2 == 0 and T.Accent or T.Accent2,
		Material = Enum.Material.Neon,
	})
	top.Parent = model

	-- Floating name + earnings sign.
	local sign = makePart({
		Name = "Sign",
		Size = Vector3.new(0.5, 0.5, 0.5),
		CFrame = cframe * CFrame.new(0, 4.5, 0),
		Transparency = 1,
		CanCollide = false,
	})
	sign.Parent = model

	local bb = Instance.new("BillboardGui")
	bb.Name = "Nameplate"
	bb.Size = UDim2.fromScale(7, 2.6)
	bb.StudsOffsetWorldSpace = Vector3.new(0, 0, 0)
	bb.AlwaysOnTop = false
	bb.MaxDistance = 80
	bb.Parent = sign

	local card = Instance.new("Frame")
	card.Size = UDim2.fromScale(1, 1)
	card.BackgroundColor3 = T.Panel
	card.BackgroundTransparency = 0.1
	card.Parent = bb
	roundify(card, 10)
	local stroke = Instance.new("UIStroke")
	stroke.Color = T.Stroke
	stroke.Thickness = 2
	stroke.Parent = card

	local nameLabel = Instance.new("TextLabel")
	nameLabel.Size = UDim2.new(1, -12, 0.55, 0)
	nameLabel.Position = UDim2.new(0, 6, 0, 2)
	nameLabel.BackgroundTransparency = 1
	nameLabel.Font = Enum.Font.GothamBold
	nameLabel.Text = player.DisplayName
	nameLabel.TextColor3 = T.Text
	nameLabel.TextScaled = true
	nameLabel.Parent = card

	local earnLabel = Instance.new("TextLabel")
	earnLabel.Name = "Earned"
	earnLabel.Size = UDim2.new(1, -12, 0.45, 0)
	earnLabel.Position = UDim2.new(0, 6, 0.55, 0)
	earnLabel.BackgroundTransparency = 1
	earnLabel.Font = Enum.Font.GothamMedium
	earnLabel.Text = "Raised: 0 R$"
	earnLabel.TextColor3 = T.Accent2
	earnLabel.TextScaled = true
	earnLabel.Parent = card

	-- Interaction prompt. The client reads OwnerUserId to open the stand UI.
	local prompt = Instance.new("ProximityPrompt")
	prompt.Name = "DonatePrompt"
	prompt.ActionText = "Donate"
	prompt.ObjectText = player.DisplayName .. "'s Stand"
	prompt.HoldDuration = 0
	prompt.MaxActivationDistance = 12
	prompt.RequiresLineOfSight = false
	prompt.Parent = base

	model.PrimaryPart = base
	model.Parent = worldFolder
	stands[player.UserId] = model
end

-- Visually flag a stand during events (spotlight / lucky).
function Stands.setHighlight(userId: number, enabled: boolean, color: Color3?)
	local model = stands[userId]
	if not model then
		return
	end
	local existing = model:FindFirstChild("EventHighlight")
	if enabled then
		local hl = existing :: Highlight
		if not hl then
			hl = Instance.new("Highlight")
			hl.Name = "EventHighlight"
			hl.FillTransparency = 0.6
			hl.OutlineTransparency = 0
			hl.DepthMode = Enum.HighlightDepthMode.AlwaysOnTop
			hl.Parent = model
		end
		hl.FillColor = color or T.Gold
		hl.OutlineColor = color or T.Gold
	elseif existing then
		existing:Destroy()
	end
end

function Stands.clearAllHighlights()
	for userId in stands do
		Stands.setHighlight(userId, false)
	end
end

function Stands.removeStand(player: Player)
	local model = stands[player.UserId]
	if model then
		local slot = model:GetAttribute("Slot")
		if type(slot) == "number" then
			slotTaken[slot] = nil
		end
		model:Destroy()
		stands[player.UserId] = nil
	end
end

-- Keep the nameplate earnings fresh.
function Stands.start()
	task.spawn(function()
		while true do
			for userId, model in stands do
				local profile = Data.get(userId)
				local label = model:FindFirstChild("Sign")
					and (model.Sign :: any).Nameplate
					and (model.Sign :: any).Nameplate:FindFirstChild("Frame")
				local earned = label and label:FindFirstChild("Earned")
				if profile and earned then
					earned.Text = "Raised: " .. Stands.comma(profile.earnedRobux) .. " R$"
				end
			end
			task.wait(4)
		end
	end)
end

return Stands
