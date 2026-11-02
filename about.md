# Click-Way-Before-Frames

An independently written Geometry Dash / Geode project.

## Interface

Open the pause-menu button to reach one compact panel with one slider: **Maximum early-click delay**, measured in physics ticks. It supports every whole-tick value from 0 to 16,384 in one-tick increments. The same global value controls both the maximum delay tested and the forward simulation horizon across modes. The initial value is 130 ticks, but users may select any value in the range.

## Full-horizon, time-sliced search

Every living candidate advances through the full configured horizon. The delay search aims for a roughly 4 ms work slice per player update, saves its progress, and resumes on later updates instead of doing the whole offset scan in the original click callback. The original input and later press/release edges are held until the search resolves.

There is no total simulation-step cap or shortened prediction horizon. One complete candidate is still an indivisible simulation run, so a single candidate at a very large horizon can exceed the 4 ms target. The selected number is never silently reduced to meet a frame-time budget.

## Current mechanics

The off-screen player clone uses Geometry Dash player update and collision routines for cube, ship, ball, UFO, wave, robot, spider and swing. Platformer jump and directional controls are mapped into the same player simulation. Press/release input duration is estimated from real input and the delayed events are queued with a shared offset.

The prediction world snapshots and restores supported object/effect activation state and group toggles. It simulates orbs, pads, gravity changes, supported vehicle portals, speed/size portals, simple teleport portals, and group toggles.

## Unverified interactions

Spawn/move/rotate trigger chains, dual/solo portals, group-parented motion, and complex force/eased teleport configurations are not fully represented. If such an interaction is encountered, the player clone continues consuming the requested horizon while alive, but the trial result is marked unverified and is never counted as a safe result. The build compiles in CI; live in-game behavior has not been runtime tested.
