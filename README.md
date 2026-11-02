# Click-Way-Before-Frames

An independently written Geometry Dash Geode mod for Windows.

Open the button from the **pause menu** to adjust one setting: **Maximum early-click delay**, in physics ticks. The slider accepts every whole-tick value from **0 to 16,384**. The selected number is used both as the maximum delay to test and as the future prediction horizon. It is shared across all player modes; there are no per-mode sliders.

- Configurable range: **0–16,384 ticks**, one-tick increments
- 4 physics ticks correspond to one 60-FPS frame
- Target: Geometry Dash **2.2081**, Windows / Geode **5.11.2**
- Initial value: **130 ticks**; this is only the first-install default, not a cap

## Prediction horizon and no-hitch search

A living candidate is advanced for the **entire selected horizon**. The search does not shorten a candidate's horizon, use a landing shortcut, or stop after repeated collision timings. A candidate that dies before its delayed input is delivered is rejected because it has no living trajectory after that point.

The delay search is time-sliced: it aims to spend about **4 ms per player update**, remembers the next offset, and resumes on a later update instead of completing the whole delay scan inside the original click callback. The original input and any subsequent press/release edges are held until the search resolves. The time slice is not a total-work cap; the scan keeps going until it finds a surviving offset, reaches the configured maximum, or encounters an unverified mechanic.

There is one important limit to that smoothing: a single candidate is still simulated as one complete run. At very large horizons (especially 16,384 ticks), that one run can itself exceed the time slice. The setting still means the full requested horizon; it is not silently reduced.

The ghost uses the game's player update and collision routines. The same physics path is used for cube, ship, ball, UFO, wave, robot, spider and swing, with the same shared setting. Platformer jump and left/right controls are also mapped into the player simulation. Each player in dual mode has a separate input queue.

## Interactions and unverified paths

The prediction engine snapshots and restores shared object activation state, effect state, group toggles, and selected moving-object state. It simulates collectible/pad interactions, gravity changes, supported vehicle portals, speed and size portals, simple teleport portals, and group toggles without committing speculative state to the real run.

Some level mechanics are not fully modeled yet, including spawn/move/rotate trigger chains, dual/solo portals, group-parented motion, and complex force/eased teleports. When one of these is encountered, the ghost still runs out the configured tick horizon where it remains alive, but the trial is marked **unverified** and is not used as a safe timing.

## Build and source

The source is independently written; the inspiration project is used only as a behavioural reference.

GitHub Actions builds the Windows package:
https://github.com/refdagew/click-way-before-frames/actions/workflows/build.yml
