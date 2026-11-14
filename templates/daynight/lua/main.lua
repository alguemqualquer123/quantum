-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- The project's entry scene is the sky showcase itself, so opening this project and pressing Play
-- (or running quantum-engine --project <this folder> with no --scene) lands on a full day: an
-- island, a lake that mirrors the dome, a clock that walks through dawn to night, a moon with a
-- real phase and a cloud deck from clear to storm.
--
-- The scene lives in engine/components/demos/demo_sky.lua because that file is the reference the
-- kit's README points at - keeping one copy means the template cannot drift from the documented
-- recipe. Keys: [ / ] hour, , / . cloud preset, - / = moon phase, 1..4 framings, SPACE pause, ESC quits.
require("demos.demo_sky")
