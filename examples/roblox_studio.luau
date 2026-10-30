-- Requires the Roblox Studio target (Roblox Integration → Connect). In the standalone runtime the
-- compatibility analyzer reports `game`, `Instance` and `Vector3` as Roblox engine dependencies.
local Workspace = game:GetService("Workspace")
local part = Instance.new("Part")
part.Name = "NexusPart"
part.Anchored = true
part.Position = Vector3.new(0, 10, 0)
part.Parent = Workspace
print("Created", part:GetFullName())
