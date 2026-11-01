# Click-Way-Before-Frames

An independent Geometry Dash / Geode project.

## Interface

The pause menu opens one compact panel with one control: **Maximum early-click delay**, measured in physics ticks. The value is shared by every mode and feeds both the delay search and forward simulation horizon. There are no separate mode-specific delay or horizon controls.

At 240 physics ticks per second, 4 ticks correspond to one 60-FPS frame. The default is 16,384 ticks = 4,096 frames = about 68.3 seconds.

## Current engine

The Windows build contains an experimental off-screen player clone. It copies the player state, simulates forward with the game's player update and collision routines, checks the immediate input, and searches later timings if the immediate input dies. A queued press/release keeps the same timing offset so the hold length is maintained.

The search is bounded to 8 ms per input and a maximum work count. On budget exhaustion or unsupported object interaction, the real input is passed through unchanged.

## Limitations / verification

- Orbs, pads, portals and triggers are not simulated by the clone yet. Prediction stops at those object-effect interactions rather than allowing the speculative player to activate shared live-level objects.
- Platformer mode is skipped.
- 16,384 ticks is about 68.3 seconds of simulated time. That is a very long horizon and is generally too costly to complete inside an 8 ms synchronous search budget; at the maximum slider value, inputs may simply pass through on many levels. This is an experimental implementation, not a guarantee that any early click can be saved.
- The code compiles and a Windows package was produced by GitHub Actions. It has not been tested in a live Geometry Dash session.