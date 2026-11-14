-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Entry point for the Waterfall Test project. The scene is built in Lua, not authored in the
-- editor, because every number in it (the lip height, the impact point, the pool sill) comes from
-- the same height function that wrote the terrain maps - see scripts/gen_terrain_maps.py.
require("demos.demo_waterfall")
