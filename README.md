# Click-Way-Before-Frames

An independently written Geometry Dash Geode mod for Windows.

Open the button from the **pause menu** to adjust one setting: **Maximum early-click delay**, in physics ticks. The same shared value controls the maximum delay candidates and the future-simulation horizon for every supported game mode. There are no per-mode sliders.

- Default / maximum: **16,384 ticks**
- At 240 physics ticks per second: **4,096 60-FPS frames**, or about **68.3 seconds**
- Target: Geometry Dash **2.2081**, Windows / Geode **5.11.2**

## What the current build does

The mod creates an off-screen player clone from the live player state, tests the current input against the future physics/collision path, and searches later input timings when the immediate timing is predicted to die. When it finds a surviving timing, it queues the press/release with a shared offset so the hold length is preserved.

The search is deliberately bounded to an **8 ms per-input budget** and a maximum simulation-work limit. When the search runs out of budget, encounters an unsupported interaction, or cannot create a clone, it sends the input through normally instead of stalling the game or delaying it on an unverified result.

## Important limitations

This is an **experimental first engine**, not a verified replacement for a mature predictive-input system:

- The clone does not yet simulate level-effect interactions such as orbs, pads, portals, and triggers. Prediction stops at those interactions and passes the input through.
- Platformer levels are not buffered.
- The default 16,384-tick horizon is about 68 seconds of simulated gameplay. Completing such a long search inside the 8 ms safety budget is generally unrealistic on complex levels; large slider values may therefore cause inputs to pass through without being delayed. Lower values are more practical until the search is redesigned as an incremental/background-safe simulation.
- It has compiled successfully for Windows, but has **not yet been tested in a live Geometry Dash session**. Test in practice mode first and expect unsupported situations to pass through.

## Build

GitHub Actions builds the Windows package. See the latest build workflow:
https://github.com/refdagew/click-way-before-frames/actions/workflows/build.yml
