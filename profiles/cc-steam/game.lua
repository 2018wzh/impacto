include(root.BasePaths.RootProfilesDir .. '/cc/game.lua');

-- Steam stores the native font atlas in two consecutive DDS strips.
-- Keep every source row, including the tail padding; glyph cells stay 48x48.
root.SpriteSheets["Font"].AppendPath = { Mount = "system", Id = 9 };
root.SpriteSheets["Font"].DesignHeight = 4096 + 1920;
root.Fonts["Default"].CellSize = { X = 48, Y = 48 };
root.Fonts["Default"].BitmapEmWidth = 48;
root.Fonts["Default"].BitmapEmHeight = 48;
