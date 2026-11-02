# Click-Way-Before-Frames

An independently written Geometry Dash / Geode project.

## Interface

Open the pause-menu button to reach one compact panel with one slider: **Maximum early-click delay**, measured in physics ticks. It supports every whole-tick value from 0 to 16,384 in one-tick increments. The same global value controls both the maximum delay tested and the forward simulation horizon across modes. The initial value is 130 ticks, but users may select any value in the range.

## Full-horizon simulation

Every living candidate advances through the full configured horizon. There is no per-click time limit, total simulation-step cap, landing shortcut, or repeated-collision shortcut. A candidate is rejected early only if its ghost dies before the scheduled input or during the future simulation; after death there is no continuing living trajectory to evaluate.

Large settings may stall the game for a substantial time, especially when many delay offsets must be tested. The mod does not reduce the requested horizon to meet a frame-time budget.

## Current mechanics

The off-screen player clone uses Geometry Dash player update and collision routines for cube, ship, ball, UFO, wave, robot, spider and swing. Platformer jump and directional controls are mapped into the same player simulation. Press/release input duration is estimated from real input and the delayed events are queued with a shared offset.

The prediction world snapshots and restores supported object/effect activation state and group toggles. It simulates orbs, pads, gravity changes, supported vehicle portals, speed/size portals, simple teleport portals, and group toggles.

## Unverified interactions

Spawn/move/rotate trigger chains, dual/solo portals, group-parented motion, and complex force/eased teleport configurations are not fully represented. If such an interaction is encountered, the player clone continues consuming the requested horizon while alive, but the trial result is marked unverified and is never counted as a safe result. The build compiles in CI; live in-game behavior has not been runtime tested.
