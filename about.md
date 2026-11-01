# Click-Way-Before-Frames

An independent Geometry Dash / Geode project.

## Interface

Open the pause-menu button to reach one compact panel with one slider: **Maximum early-click delay**, measured in physics ticks. It supports every whole-tick value from 0 to 16,384 in one-tick increments. The selected value is global, shared across player modes, and used for both the delay-search ceiling and the forward prediction horizon.

Four physics ticks correspond to one 60-FPS frame. This conversion is only informational; the slider itself is tick-based.

## Engine

The Windows build uses an off-screen player clone and the game's player update and collision routines. It first tests the immediate input, then checks delayed timings. Press and release events use the same queued offset to preserve the held duration.

The same player-physics path is used for cube, ship, ball, UFO, wave, robot, spider, and swing. Platformer jump and left/right controls are mapped to the corresponding player buttons. Each player in dual mode has a separate input queue.

The search has an 8 ms time budget and a maximum simulation-work cap. If it cannot finish, it leaves the real input unchanged rather than applying an unverified delay.

## Known boundaries

- The speculative player still shares the live level's objects. Prediction therefore stops at level-effect interactions (including orbs, pads, portals, and triggers) rather than allowing a test run to mutate the actual level.
- Large horizons are expensive. The selected slider amount remains the configured limit and horizon, but a computation that exceeds the safety budget passes the input through.
- The project builds as a Windows .geode package. Runtime behavior has not been verified inside Geometry Dash yet.
