# Click-Way-Before-Frames

An independent Geometry Dash / Geode project.

## Interface goal

The pause menu opens one compact panel with one control: **Maximum early-click delay**, measured in physics ticks. The value is shared by every game mode. The intended prediction horizon uses that same value; there are no separate per-mode horizon controls.

At 240 physics steps per second, 4 ticks correspond to one 60 FPS frame. The default slider value is 16,384 ticks (4,096 frames).

## Build

GitHub Actions builds a Windows .geode package with the Geode SDK. Download it from the successful workflow run.

## Development status

This first commit establishes the independent mod shell, pause-menu entry point, and persisted one-slider configuration. The collision-aware future simulation and input scheduling engine are not implemented yet; the slider alone does not currently delay or move gameplay inputs. This is not a finished predictive-input mod.
