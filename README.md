# Click-Way-Before-Frames

An independently written Geometry Dash Geode mod for Windows.

Open the button from the **pause menu** to adjust one setting: **Maximum early-click delay**, in physics ticks. The slider accepts any whole-tick value from **0 to 16,384**; it is not locked to a preset duration. The same value controls both the delay search and the forward-simulation horizon. It is shared across all player modes; there are no per-mode sliders.

- Configurable range: **0–16,384 ticks**, one-tick increments
- 4 physics ticks correspond to one 60-FPS frame
- Target: Geometry Dash **2.2081**, Windows / Geode **5.11.2**

## Current engine

The mod creates an off-screen player clone from the live player state, tests the current input against the future physics/collision path, and searches later input timings when the immediate timing is predicted to die. When it finds a surviving timing, it queues the press/release with a shared offset so the hold length is preserved.

The prediction path uses the same player physics for the classic modes (cube, ship, ball, UFO, wave, robot, spider and swing). Platformer jump and left/right inputs are also mapped into that player simulation and use the same slider. The dual-player queue is maintained separately for each player.

The search is deliberately bounded to an **8 ms per-input budget** and a maximum simulation-work limit. When the search runs out of budget, encounters an unsupported interaction, or cannot create a clone, it sends the input through normally instead of stalling the game or delaying it on an unverified result.

## Known boundaries

- The clone still shares the live level's object collection. To avoid speculative inputs activating shared level state, prediction stops at level-effect interactions such as orbs, pads, portals, and triggers; inputs at those points pass through normally.
- Long horizons have substantial simulation cost. The configured number is honored as the search cap and horizon, but a search that cannot finish inside the safety budget passes through normally.
- The Windows package compiles successfully. It has not been run inside a live Geometry Dash session, so in-game behavior still needs validation.

## Build

GitHub Actions builds the Windows package. See the latest build workflow:
https://github.com/refdagew/click-way-before-frames/actions/workflows/build.yml
