--!strict
--[[
	DonationCentral - client
	------------------------
	Builds the entire UI in code (no imported images/decals) and wires it to the
	server remotes. Panels: My Stand, Donate, Leaderboards and an owner-only
	Dashboard, plus toast notifications and the live-event banner + countdown.
]]

local Players = game:GetService("Players")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local TweenService = game:GetService("TweenService")
local ProximityPromptService = game:GetService("ProximityPromptService")

local LocalPlayer = Players.LocalPlayer
local playerGui = LocalPlayer:WaitForChild("PlayerGui")

local shared = ReplicatedStorage:WaitForChild("DonationShared")
local Config = require(shared:WaitForChild("Config"))
local Net = require(shared:WaitForChild("Net"))

local T = Config.Theme

-- ----------------------------------------------------------------- UI helpers
local function new(class: string, props: { [string]: any }?, children: { Instance }?): any
	local inst = Instance.new(class)
	if props then
		for k, v in props do
			(inst :: any)[k] = v
		end
	end
	if children then
		for _, c in children do
			c.Parent = inst
		end
	end
	return inst
end

local function corner(radius: number): UICorner
	return new("UICorner", { CornerRadius = UDim.new(0, radius) })
end

local function stroke(color: Color3, thickness: number?): UIStroke
	return new("UIStroke", { Color = color, Thickness = thickness or 1, Transparency = 0.2 })
end

local function pad(all: number): UIPadding
	return new("UIPadding", {
		PaddingTop = UDim.new(0, all),
		PaddingBottom = UDim.new(0, all),
		PaddingLeft = UDim.new(0, all),
		PaddingRight = UDim.new(0, all),
	})
end

local function comma(n: number): string
	local s = tostring(math.floor(tonumber(n) or 0))
	local out = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
	return (out:gsub("^,", ""))
end

local function button(text: string, color: Color3, size: UDim2): TextButton
	return new("TextButton", {
		Text = text,
		Font = Enum.Font.GothamBold,
		TextColor3 = T.Text,
		TextScaled = true,
		BackgroundColor3 = color,
		Size = size,
		AutoButtonColor = true,
	}, { corner(8), pad(6) })
end

-- --------------------------------------------------------------- root + layout
local gui = new("ScreenGui", {
	Name = "DonationCentralUI",
	ResetOnSpawn = false,
	ZIndexBehavior = Enum.ZIndexBehavior.Sibling,
	IgnoreGuiInset = true,
})
gui.Parent = playerGui

-- Top bar
local topBar = new("Frame", {
	Size = UDim2.new(1, 0, 0, 56),
	BackgroundColor3 = T.Panel,
}, { stroke(T.Stroke) })
topBar.Parent = gui

local logo = new("TextLabel", {
	Size = UDim2.new(0, 320, 1, 0),
	Position = UDim2.fromOffset(16, 0),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamBlack,
	Text = "DONATION CENTRAL",
	TextColor3 = T.Text,
	TextXAlignment = Enum.TextXAlignment.Left,
	TextSize = 24,
})
logo.Parent = topBar

local navHolder = new("Frame", {
	Size = UDim2.new(1, -340, 1, -14),
	Position = UDim2.new(0, 336, 0, 7),
	BackgroundTransparency = 1,
})
navHolder.Parent = topBar
new("UIListLayout", {
	FillDirection = Enum.FillDirection.Horizontal,
	HorizontalAlignment = Enum.HorizontalAlignment.Right,
	VerticalAlignment = Enum.VerticalAlignment.Center,
	Padding = UDim.new(0, 8),
}).Parent = navHolder

-- Content area holding the panels
local content = new("Frame", {
	Size = UDim2.new(1, -32, 1, -88),
	Position = UDim2.fromOffset(16, 72),
	BackgroundTransparency = 1,
})
content.Parent = gui

local panels: { [string]: Frame } = {}
local navButtons: { [string]: TextButton } = {}

local function makePanel(name: string): Frame
	local p = new("Frame", {
		Name = name,
		Size = UDim2.fromScale(1, 1),
		BackgroundColor3 = T.Background,
		BackgroundTransparency = 0.15,
		Visible = false,
	}, { corner(14), stroke(T.Stroke), pad(16) })
	p.Parent = content
	panels[name] = p
	return p
end

local function showPanel(name: string)
	for key, panel in panels do
		panel.Visible = (key == name)
	end
	for key, btn in navButtons do
		btn.BackgroundColor3 = (key == name) and T.Accent or T.Panel2
	end
end

local function addNav(name: string, label: string)
	local btn = button(label, T.Panel2, UDim2.fromOffset(130, 1))
	btn.Size = UDim2.new(0, 130, 1, 0)
	btn.TextSize = 15
	btn.TextScaled = false
	btn.Parent = navHolder
	btn.Activated:Connect(function()
		showPanel(name)
	end)
	navButtons[name] = btn
end

-- ------------------------------------------------------------------- toasts
local toastHolder = new("Frame", {
	Size = UDim2.new(0, 320, 1, -80),
	Position = UDim2.new(1, -336, 0, 72),
	BackgroundTransparency = 1,
})
toastHolder.Parent = gui
new("UIListLayout", {
	VerticalAlignment = Enum.VerticalAlignment.Bottom,
	HorizontalAlignment = Enum.HorizontalAlignment.Right,
	Padding = UDim.new(0, 8),
	SortOrder = Enum.SortOrder.LayoutOrder,
}).Parent = toastHolder

local toastId = 0
local function toast(kind: string, text: string)
	toastId += 1
	local color = T.Accent
	if kind == "good" then
		color = T.Good
	elseif kind == "bad" then
		color = T.Bad
	elseif kind == "warn" then
		color = T.Warn
	end

	local card = new("Frame", {
		Size = UDim2.new(1, 0, 0, 0),
		AutomaticSize = Enum.AutomaticSize.Y,
		BackgroundColor3 = T.Panel,
		LayoutOrder = toastId,
		BackgroundTransparency = 1,
	}, {
		corner(10),
		stroke(color, 2),
		pad(10),
		new("TextLabel", {
			Size = UDim2.new(1, 0, 0, 0),
			AutomaticSize = Enum.AutomaticSize.Y,
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamMedium,
			Text = text,
			TextColor3 = T.Text,
			TextSize = 15,
			TextWrapped = true,
			TextXAlignment = Enum.TextXAlignment.Left,
		}),
	})
	card.Parent = toastHolder

	TweenService:Create(card, TweenInfo.new(0.2), { BackgroundTransparency = 0.05 }):Play()
	task.delay(5, function()
		TweenService:Create(card, TweenInfo.new(0.3), { BackgroundTransparency = 1 }):Play()
		task.wait(0.3)
		card:Destroy()
	end)
end

-- ------------------------------------------------------------- event banner
local eventBanner = new("Frame", {
	Size = UDim2.new(0, 460, 0, 70),
	Position = UDim2.new(0.5, -230, 0, 64),
	BackgroundColor3 = T.Panel,
	Visible = false,
}, { corner(12), stroke(T.Accent, 2), pad(10) })
eventBanner.Parent = gui

local eventTitle = new("TextLabel", {
	Size = UDim2.new(1, 0, 0, 24),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamBlack,
	Text = "",
	TextColor3 = T.Text,
	TextScaled = true,
})
eventTitle.Parent = eventBanner

local eventBlurb = new("TextLabel", {
	Size = UDim2.new(1, 0, 0, 20),
	Position = UDim2.fromOffset(0, 26),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamMedium,
	Text = "",
	TextColor3 = T.SubText,
	TextSize = 13,
	TextWrapped = true,
})
eventBlurb.Parent = eventBanner

local eventTimer = new("TextLabel", {
	Size = UDim2.new(1, 0, 0, 16),
	Position = UDim2.fromOffset(0, 50),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamBold,
	Text = "",
	TextColor3 = T.Accent2,
	TextSize = 13,
})
eventTimer.Parent = eventBanner

local eventCountToken = 0
local function startEvent(payload)
	eventCountToken += 1
	local myToken = eventCountToken
	eventBanner.Visible = true
	local bannerStroke = eventBanner:FindFirstChildOfClass("UIStroke")
	if bannerStroke then
		bannerStroke.Color = payload.accent or T.Accent
	end
	eventTitle.Text = payload.title or "EVENT"
	eventTitle.TextColor3 = payload.accent or T.Text
	eventBlurb.Text = payload.blurb or ""
	if payload.targetName then
		eventBlurb.Text ..= "  →  " .. payload.targetName
	end

	local remaining = tonumber(payload.duration) or 0
	task.spawn(function()
		while remaining > 0 and myToken == eventCountToken do
			eventTimer.Text = ("⏳ %d:%02d left"):format(math.floor(remaining / 60), remaining % 60)
			task.wait(1)
			remaining -= 1
		end
	end)
end

local function endEvent(payload)
	eventCountToken += 1
	eventTitle.Text = payload.title or "EVENT"
	eventBlurb.Text = payload.resultText or "Event over!"
	eventTimer.Text = "Finished"
	task.delay(6, function()
		if eventTimer.Text == "Finished" then
			eventBanner.Visible = false
		end
	end)
end

-- =========================================================== MY STAND panel
local myStand = makePanel("MyStand")
new("UIListLayout", { Padding = UDim.new(0, 10), SortOrder = Enum.SortOrder.LayoutOrder }).Parent = myStand

new("TextLabel", {
	LayoutOrder = 1,
	Size = UDim2.new(1, 0, 0, 30),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamBlack,
	Text = "My Stand",
	TextColor3 = T.Text,
	TextXAlignment = Enum.TextXAlignment.Left,
	TextSize = 24,
}).Parent = myStand

-- Stats strip
local statStrip = new("Frame", {
	LayoutOrder = 2,
	Size = UDim2.new(1, 0, 0, 72),
	BackgroundTransparency = 1,
})
statStrip.Parent = myStand
new("UIListLayout", {
	FillDirection = Enum.FillDirection.Horizontal,
	Padding = UDim.new(0, 10),
}).Parent = statStrip

local function statCard(title: string, color: Color3): TextLabel
	local card = new("Frame", {
		Size = UDim2.new(0.25, -8, 1, 0),
		BackgroundColor3 = T.Panel,
	}, { corner(10), stroke(T.Stroke), pad(8) })
	card.Parent = statStrip
	new("TextLabel", {
		Size = UDim2.new(1, 0, 0, 16),
		BackgroundTransparency = 1,
		Font = Enum.Font.GothamMedium,
		Text = title,
		TextColor3 = T.SubText,
		TextSize = 12,
		TextXAlignment = Enum.TextXAlignment.Left,
	}).Parent = card
	local value = new("TextLabel", {
		Size = UDim2.new(1, 0, 1, -16),
		Position = UDim2.fromOffset(0, 16),
		BackgroundTransparency = 1,
		Font = Enum.Font.GothamBlack,
		Text = "0",
		TextColor3 = color,
		TextScaled = true,
		TextXAlignment = Enum.TextXAlignment.Left,
	})
	value.Parent = card
	return value
end

local statEarned = statCard("Robux Raised", T.Accent2)
local statDonated = statCard("Robux Donated", T.Accent)
local statRecv = statCard("Donations Received", T.Gold)
local statGiven = statCard("Donations Given", T.Text)

-- Add-pass row
local addRow = new("Frame", {
	LayoutOrder = 3,
	Size = UDim2.new(1, 0, 0, 44),
	BackgroundTransparency = 1,
})
addRow.Parent = myStand

local passInput = new("TextBox", {
	Size = UDim2.new(1, -150, 1, 0),
	BackgroundColor3 = T.Panel2,
	Font = Enum.Font.GothamMedium,
	PlaceholderText = "Paste your Game Pass ID (numbers only)",
	Text = "",
	TextColor3 = T.Text,
	PlaceholderColor3 = T.SubText,
	TextSize = 15,
	ClearTextOnFocus = false,
}, { corner(8), pad(8) })
passInput.Parent = addRow

local addBtn = button("+ Link Pass", T.Accent2, UDim2.new(0, 140, 1, 0))
addBtn.Position = UDim2.new(1, -140, 0, 0)
addBtn.Parent = addRow

new("TextLabel", {
	LayoutOrder = 4,
	Size = UDim2.new(1, 0, 0, 30),
	BackgroundTransparency = 1,
	Font = Enum.Font.Gotham,
	Text = ("Tip: create a Game Pass you OWN, set it On Sale, then paste its ID. Buyers pay you directly through Roblox (you keep ~%d%% after Roblox's fee)."):format(
		math.floor((1 - Config.MarketplaceFee) * 100)
	),
	TextColor3 = T.SubText,
	TextSize = 12,
	TextWrapped = true,
	TextXAlignment = Enum.TextXAlignment.Left,
}).Parent = myStand

local passList = new("ScrollingFrame", {
	LayoutOrder = 5,
	Size = UDim2.new(1, 0, 1, -230),
	BackgroundTransparency = 1,
	ScrollBarThickness = 6,
	CanvasSize = UDim2.new(),
	AutomaticCanvasSize = Enum.AutomaticSize.Y,
})
passList.Parent = myStand
new("UIListLayout", { Padding = UDim.new(0, 8), SortOrder = Enum.SortOrder.LayoutOrder }).Parent = passList

local function renderMyProfile(profile)
	if not profile then
		return
	end
	statEarned.Text = comma(profile.earnedRobux) .. " R$"
	statDonated.Text = comma(profile.donatedRobux) .. " R$"
	statRecv.Text = comma(profile.receivedCount)
	statGiven.Text = comma(profile.donationCount)

	for _, child in passList:GetChildren() do
		if child:IsA("Frame") then
			child:Destroy()
		end
	end

	if #profile.linkedPasses == 0 then
		new("TextLabel", {
			Size = UDim2.new(1, 0, 0, 40),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamMedium,
			Text = "No passes linked yet. Add one above to start earning!",
			TextColor3 = T.SubText,
			TextSize = 14,
		}).Parent = passList
		return
	end

	for i, p in profile.linkedPasses do
		local row = new("Frame", {
			LayoutOrder = i,
			Size = UDim2.new(1, 0, 0, 52),
			BackgroundColor3 = T.Panel,
		}, { corner(10), stroke(T.Stroke), pad(8) })
		row.Parent = passList

		new("TextLabel", {
			Size = UDim2.new(1, -220, 1, 0),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamBold,
			Text = p.name,
			TextColor3 = T.Text,
			TextSize = 15,
			TextTruncate = Enum.TextTruncate.AtEnd,
			TextXAlignment = Enum.TextXAlignment.Left,
		}).Parent = row

		new("TextLabel", {
			Size = UDim2.new(0, 120, 1, 0),
			Position = UDim2.new(1, -210, 0, 0),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamBlack,
			Text = comma(p.price) .. " R$",
			TextColor3 = T.Accent2,
			TextSize = 16,
			TextXAlignment = Enum.TextXAlignment.Right,
		}).Parent = row

		local removeBtn = button("Unlink", T.Bad, UDim2.new(0, 80, 1, 0))
		removeBtn.Position = UDim2.new(1, -80, 0, 0)
		removeBtn.TextScaled = false
		removeBtn.TextSize = 14
		removeBtn.Parent = row
		removeBtn.Activated:Connect(function()
			Net.func("UnregisterPass"):InvokeServer(p.id)
		end)
	end
end

addBtn.Activated:Connect(function()
	local id = passInput.Text:gsub("%s", "")
	if id == "" then
		return
	end
	addBtn.Text = "..."
	local ok, result = pcall(function()
		return Net.func("RegisterPass"):InvokeServer(id)
	end)
	addBtn.Text = "+ Link Pass"
	if ok and result and result.ok then
		passInput.Text = ""
		toast("good", "Linked " .. (result.pass and result.pass.name or "pass") .. "!")
	elseif ok and result then
		toast("bad", result.err or "Couldn't link that pass.")
	else
		toast("bad", "Something went wrong - try again.")
	end
end)

-- =========================================================== DONATE panel
local donate = makePanel("Donate")
new("TextLabel", {
	Size = UDim2.new(1, 0, 0, 30),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamBlack,
	Text = "Donate to a Player",
	TextColor3 = T.Text,
	TextXAlignment = Enum.TextXAlignment.Left,
	TextSize = 24,
}).Parent = donate

-- left: player list, right: selected stand
local playerScroll = new("ScrollingFrame", {
	Size = UDim2.new(0.4, -8, 1, -44),
	Position = UDim2.fromOffset(0, 40),
	BackgroundColor3 = T.Panel,
	BackgroundTransparency = 0.3,
	ScrollBarThickness = 6,
	CanvasSize = UDim2.new(),
	AutomaticCanvasSize = Enum.AutomaticSize.Y,
}, { corner(10), pad(8) })
playerScroll.Parent = donate
new("UIListLayout", { Padding = UDim.new(0, 6), SortOrder = Enum.SortOrder.Name }).Parent = playerScroll

local standView = new("Frame", {
	Size = UDim2.new(0.6, -8, 1, -44),
	Position = UDim2.new(0.4, 8, 0, 40),
	BackgroundColor3 = T.Panel,
	BackgroundTransparency = 0.3,
}, { corner(10), pad(12) })
standView.Parent = donate

local standTitle = new("TextLabel", {
	Size = UDim2.new(1, 0, 0, 28),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamBlack,
	Text = "Select a player",
	TextColor3 = T.Text,
	TextXAlignment = Enum.TextXAlignment.Left,
	TextSize = 20,
})
standTitle.Parent = standView

local standSub = new("TextLabel", {
	Size = UDim2.new(1, 0, 0, 18),
	Position = UDim2.fromOffset(0, 28),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamMedium,
	Text = "Their linked passes appear here.",
	TextColor3 = T.SubText,
	TextSize = 13,
	TextXAlignment = Enum.TextXAlignment.Left,
})
standSub.Parent = standView

local standPasses = new("ScrollingFrame", {
	Size = UDim2.new(1, 0, 1, -56),
	Position = UDim2.fromOffset(0, 54),
	BackgroundTransparency = 1,
	ScrollBarThickness = 6,
	CanvasSize = UDim2.new(),
	AutomaticCanvasSize = Enum.AutomaticSize.Y,
})
standPasses.Parent = standView
new("UIListLayout", { Padding = UDim.new(0, 8) }).Parent = standPasses

local function openStand(userId: number)
	local data = Net.func("GetStand"):InvokeServer(userId)
	for _, c in standPasses:GetChildren() do
		if c:IsA("Frame") then
			c:Destroy()
		end
	end
	if not data then
		standTitle.Text = "Stand unavailable"
		standSub.Text = "That player isn't available right now."
		return
	end
	standTitle.Text = data.name .. "'s Stand"
	standSub.Text = ("Raised %s R$  •  %d donations received"):format(comma(data.earnedRobux), data.receivedCount)

	if #data.passes == 0 then
		new("TextLabel", {
			Size = UDim2.new(1, 0, 0, 40),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamMedium,
			Text = "This player hasn't linked any passes yet.",
			TextColor3 = T.SubText,
			TextSize = 14,
		}).Parent = standPasses
		return
	end

	for i, p in data.passes do
		local row = new("Frame", {
			LayoutOrder = i,
			Size = UDim2.new(1, 0, 0, 54),
			BackgroundColor3 = T.Panel2,
		}, { corner(10), pad(8) })
		row.Parent = standPasses
		new("TextLabel", {
			Size = UDim2.new(1, -150, 1, 0),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamBold,
			Text = p.name,
			TextColor3 = T.Text,
			TextSize = 15,
			TextTruncate = Enum.TextTruncate.AtEnd,
			TextXAlignment = Enum.TextXAlignment.Left,
		}).Parent = row
		local donateBtn = button(comma(p.price) .. " R$", T.Accent2, UDim2.new(0, 140, 1, 0))
		donateBtn.Position = UDim2.new(1, -140, 0, 0)
		donateBtn.TextScaled = false
		donateBtn.TextSize = 16
		donateBtn.Parent = row
		donateBtn.Activated:Connect(function()
			Net.event("RequestDonation"):FireServer(userId, p.id)
		end)
	end
end

local function refreshPlayerList()
	for _, c in playerScroll:GetChildren() do
		if c:IsA("TextButton") then
			c:Destroy()
		end
	end
	for _, plr in Players:GetPlayers() do
		if plr ~= LocalPlayer then
			local b = button(plr.DisplayName, T.Panel2, UDim2.new(1, 0, 0, 40))
			b.Name = plr.Name
			b.TextScaled = false
			b.TextSize = 15
			b.TextXAlignment = Enum.TextXAlignment.Left
			b.Parent = playerScroll
			local uid = plr.UserId
			b.Activated:Connect(function()
				openStand(uid)
			end)
		end
	end
end

Players.PlayerAdded:Connect(refreshPlayerList)
Players.PlayerRemoving:Connect(function()
	task.defer(refreshPlayerList)
end)

-- =========================================================== LEADERBOARDS panel
local lb = makePanel("Leaderboards")
new("TextLabel", {
	Size = UDim2.new(1, 0, 0, 30),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamBlack,
	Text = "Leaderboards",
	TextColor3 = T.Text,
	TextXAlignment = Enum.TextXAlignment.Left,
	TextSize = 24,
}).Parent = lb

local function lbColumn(title: string, accent: Color3, xScale: number): ScrollingFrame
	local col = new("Frame", {
		Size = UDim2.new(0.5, -8, 1, -44),
		Position = UDim2.new(xScale, xScale == 0 and 0 or 8, 0, 40),
		BackgroundColor3 = T.Panel,
		BackgroundTransparency = 0.3,
	}, { corner(10), pad(10) })
	col.Parent = lb
	new("TextLabel", {
		Size = UDim2.new(1, 0, 0, 26),
		BackgroundColor3 = accent,
		Font = Enum.Font.GothamBlack,
		Text = title,
		TextColor3 = Color3.fromRGB(20, 22, 32),
		TextSize = 16,
	}, { corner(8) }).Parent = col
	local scroll = new("ScrollingFrame", {
		Size = UDim2.new(1, 0, 1, -34),
		Position = UDim2.fromOffset(0, 32),
		BackgroundTransparency = 1,
		ScrollBarThickness = 6,
		CanvasSize = UDim2.new(),
		AutomaticCanvasSize = Enum.AutomaticSize.Y,
	})
	scroll.Parent = col
	new("UIListLayout", { Padding = UDim.new(0, 6) }).Parent = scroll
	return scroll
end

local earnersCol = lbColumn("🏆 TOP EARNERS", T.Accent2, 0)
local donorsCol = lbColumn("💎 TOP DONORS", T.Accent, 0.5)

local function renderBoardColumn(scroll: ScrollingFrame, entries, accent: Color3)
	for _, c in scroll:GetChildren() do
		if c:IsA("Frame") then
			c:Destroy()
		end
	end
	if not entries or #entries == 0 then
		new("TextLabel", {
			Size = UDim2.new(1, 0, 0, 40),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamMedium,
			Text = "No entries yet.",
			TextColor3 = T.SubText,
			TextSize = 14,
		}).Parent = scroll
		return
	end
	for rank, e in entries do
		local medal = T.Text
		if rank == 1 then
			medal = T.Gold
		elseif rank == 2 then
			medal = T.Silver
		elseif rank == 3 then
			medal = T.Bronze
		end
		local row = new("Frame", {
			LayoutOrder = rank,
			Size = UDim2.new(1, 0, 0, 36),
			BackgroundColor3 = T.Panel2,
		}, { corner(8), pad(6) })
		row.Parent = scroll
		new("TextLabel", {
			Size = UDim2.new(0, 36, 1, 0),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamBlack,
			Text = "#" .. rank,
			TextColor3 = medal,
			TextSize = 15,
			TextXAlignment = Enum.TextXAlignment.Left,
		}).Parent = row
		new("TextLabel", {
			Size = UDim2.new(1, -150, 1, 0),
			Position = UDim2.fromOffset(40, 0),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamBold,
			Text = e.name,
			TextColor3 = T.Text,
			TextSize = 14,
			TextTruncate = Enum.TextTruncate.AtEnd,
			TextXAlignment = Enum.TextXAlignment.Left,
		}).Parent = row
		new("TextLabel", {
			Size = UDim2.new(0, 110, 1, 0),
			Position = UDim2.new(1, -110, 0, 0),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamBlack,
			Text = comma(e.value) .. " R$",
			TextColor3 = accent,
			TextSize = 14,
			TextXAlignment = Enum.TextXAlignment.Right,
		}).Parent = row
	end
end

local function renderLeaderboards(snapshot)
	if not snapshot then
		return
	end
	renderBoardColumn(earnersCol, snapshot.topEarners, T.Accent2)
	renderBoardColumn(donorsCol, snapshot.topDonors, T.Accent)
end

-- =========================================================== DASHBOARD (owner)
local dash = makePanel("Dashboard")
new("TextLabel", {
	Size = UDim2.new(1, -120, 0, 30),
	BackgroundTransparency = 1,
	Font = Enum.Font.GothamBlack,
	Text = "Owner Dashboard",
	TextColor3 = T.Text,
	TextXAlignment = Enum.TextXAlignment.Left,
	TextSize = 24,
}).Parent = dash

local refreshDash = button("Refresh", T.Accent, UDim2.new(0, 110, 0, 32))
refreshDash.Position = UDim2.new(1, -110, 0, 0)
refreshDash.TextScaled = false
refreshDash.TextSize = 14
refreshDash.Parent = dash

local dashBody = new("ScrollingFrame", {
	Size = UDim2.new(1, 0, 1, -44),
	Position = UDim2.fromOffset(0, 40),
	BackgroundTransparency = 1,
	ScrollBarThickness = 6,
	CanvasSize = UDim2.new(),
	AutomaticCanvasSize = Enum.AutomaticSize.Y,
})
dashBody.Parent = dash
new("UIListLayout", { Padding = UDim.new(0, 8) }).Parent = dashBody

local function renderDashboard()
	for _, c in dashBody:GetChildren() do
		if c:IsA("Frame") or c:IsA("TextLabel") then
			c:Destroy()
		end
	end
	local stats = Net.func("GetAdminStats"):InvokeServer()
	if not stats or not stats.ok then
		new("TextLabel", {
			Size = UDim2.new(1, 0, 0, 40),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamMedium,
			Text = (stats and stats.err) or "Owner only.",
			TextColor3 = T.Bad,
			TextSize = 15,
		}).Parent = dashBody
		return
	end

	new("TextLabel", {
		LayoutOrder = 1,
		Size = UDim2.new(1, 0, 0, 24),
		BackgroundTransparency = 1,
		Font = Enum.Font.GothamBold,
		Text = ("This server: %s R$ donated across %d donations • %d players online")
			:format(comma(stats.session.donated), stats.session.count, stats.session.players),
		TextColor3 = T.Accent2,
		TextSize = 15,
		TextXAlignment = Enum.TextXAlignment.Left,
	}).Parent = dashBody

	new("TextLabel", {
		LayoutOrder = 2,
		Size = UDim2.new(1, 0, 0, 36),
		BackgroundTransparency = 1,
		Font = Enum.Font.Gotham,
		Text = ("Note: gamepass donations pay each player directly through Roblox (they keep ~%d%% after the %d%% fee). There's nothing to pay out by hand - this is a performance view.")
			:format(100 - stats.feePct, stats.feePct),
		TextColor3 = T.SubText,
		TextSize = 12,
		TextWrapped = true,
		TextXAlignment = Enum.TextXAlignment.Left,
	}).Parent = dashBody

	new("TextLabel", {
		LayoutOrder = 3,
		Size = UDim2.new(1, 0, 0, 22),
		BackgroundTransparency = 1,
		Font = Enum.Font.GothamBold,
		Text = "Online players (by Robux raised)",
		TextColor3 = T.Text,
		TextSize = 14,
		TextXAlignment = Enum.TextXAlignment.Left,
	}).Parent = dashBody

	for i, row in stats.online do
		local card = new("Frame", {
			LayoutOrder = 3 + i,
			Size = UDim2.new(1, 0, 0, 34),
			BackgroundColor3 = T.Panel2,
		}, { corner(8), pad(6) })
		card.Parent = dashBody
		new("TextLabel", {
			Size = UDim2.new(0.5, 0, 1, 0),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamBold,
			Text = row.name,
			TextColor3 = T.Text,
			TextSize = 14,
			TextXAlignment = Enum.TextXAlignment.Left,
		}).Parent = card
		new("TextLabel", {
			Size = UDim2.new(0.5, -8, 1, 0),
			Position = UDim2.new(0.5, 0, 0, 0),
			BackgroundTransparency = 1,
			Font = Enum.Font.GothamMedium,
			Text = ("raised %s R$ • donated %s R$ • %d passes")
				:format(comma(row.earnedRobux), comma(row.donatedRobux), row.linkedPasses),
			TextColor3 = T.SubText,
			TextSize = 12,
			TextXAlignment = Enum.TextXAlignment.Right,
		}).Parent = card
	end
end

refreshDash.Activated:Connect(renderDashboard)

-- ------------------------------------------------------------------- nav setup
addNav("MyStand", "My Stand")
addNav("Donate", "Donate")
addNav("Leaderboards", "Leaderboards")

-- Show the dashboard tab only to owners.
task.spawn(function()
	local ok, stats = pcall(function()
		return Net.func("GetAdminStats"):InvokeServer()
	end)
	if ok and stats and stats.ok then
		addNav("Dashboard", "Dashboard")
	end
end)

-- ------------------------------------------------------------------- remotes
Net.event("Notify").OnClientEvent:Connect(function(payload)
	if type(payload) == "table" then
		toast(payload.kind or "info", payload.text or "")
	end
end)

Net.event("ProfileUpdated").OnClientEvent:Connect(renderMyProfile)
Net.event("LeaderboardUpdated").OnClientEvent:Connect(renderLeaderboards)

Net.event("GameEvent").OnClientEvent:Connect(function(payload)
	if type(payload) ~= "table" then
		return
	end
	if payload.phase == "start" then
		startEvent(payload)
		toast("info", payload.title .. " started!")
	elseif payload.phase == "end" then
		endEvent(payload)
	end
end)

-- Walk-up stands: open the Donate panel for a stand when its prompt triggers.
ProximityPromptService.PromptTriggered:Connect(function(prompt, player)
	if player ~= LocalPlayer or prompt.Name ~= "DonatePrompt" then
		return
	end
	local model = prompt.Parent and prompt.Parent.Parent
	local ownerId = model and model:GetAttribute("OwnerUserId")
	if typeof(ownerId) == "number" then
		showPanel("Donate")
		openStand(ownerId)
	end
end)

-- ------------------------------------------------------------------- startup
refreshPlayerList()
renderMyProfile(Net.func("GetProfile"):InvokeServer())
renderLeaderboards(Net.func("GetLeaderboards"):InvokeServer())
showPanel("MyStand")

print("[DonationCentral] Client UI ready.")
