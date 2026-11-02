# Click-Way-Before-Frames

An independently written Geometry Dash Geode mod for Windows.

Open the button from the **pause menu** to adjust one setting: **Maximum early-click delay**, in physics ticks. The slider accepts every whole-tick value from **0 to 16,384**. The selected number is used both as the maximum delay to test and as the future prediction horizon. It is shared across all player modes; there are no per-mode sliders.

- Configurable range: **0–16,384 ticks**, one-tick increments
- 4 physics ticks correspond to one 60-FPS frame
- Target: Geometry Dash **2.2081**, Windows / Geode **5.11.2**
- Initial value: **130 ticks**; this is only the first-install default, not a cap

## Prediction horizon

A living candidate is advanced for the **entire selected horizon**. There is no wall-clock deadline, simulation-work ceiling, landing shortcut, or repeated-collision shortcut. A candidate that dies before its delayed input is delivered is rejected because it has no living trajectory after that point.

This means large values can take a long time to evaluate. At 16,384 ticks, a click that requires testing many possible offsets can stall the game for a substantial period. The mod does not silently shorten the selected horizon to protect frame rate.

The ghost uses the game's player update and collision routines. The same physics path is used for cube, ship, ball, UFO, wave, robot, spider and swing, with the same shared setting. Platformer jump and left/right controls are also mapped into the player simulation. Each player in dual mode has a separate input queue.

## Interactions and unverified paths

The prediction engine snapshots and restores shared object activation state, effect state, group toggles, and selected moving-object state. It simulates collectible/pad interactions, gravity changes, supported vehicle portals, speed and size portals, simple teleport portals, and group toggles without committing speculative state to the real run.

Some level mechanics are not fully modeled yet, including spawn/move/rotate trigger chains, dual/solo portals, group-parented motion, and complex force/eased teleports. When one of these is encountered, the ghost still runs out the configured tick horizon where it remains alive, but the trial is marked **unverified** and is not used as a safe timing.

## Build and source

The source is independently written; the inspiration project is used only as a behavioural reference.

GitHub Actions builds the Windows package:
https://github.com/refdagew/click-way-before-frames/actions/workflows/build.yml
