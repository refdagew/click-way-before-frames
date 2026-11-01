# Click-Way-Before-Frames

An independently written Geometry Dash Geode project.

The intended experience is deliberately simple: open the panel from the pause menu and adjust one shared maximum early-click delay in ticks. The same value is intended to govern both the delay search and prediction horizon for every game mode.

- Default: **16,384 ticks**
- At 240 physics steps/sec: **4,096 frames at 60 FPS**
- Target: Geometry Dash **2.2081**, Windows / Geode **5.11.2**

The project includes a GitHub Actions workflow for producing a Windows .geode build.

**Status:** the current prototype adds the pause-menu panel and persists the single slider value. It does not yet contain the collision-aware prediction/input scheduling engine, so it is not yet a working early-click buffer.
