# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-07-01

First release of the native C++ / libobs telestrator.

### Engine
- **Telestrator source** (`telestrator`): canvas + preview render-target
  architecture with stroke history (undo / redo by replay), temp strokes
  (laser, auto-fade), and an optional armed-indicator dot.
- **Broadcast-quality rendering:** supersampled anti-aliased ink, solid filled
  arrowheads, continuous evenly-spaced dashes (shapes dash properly around
  corners), uniform-alpha translucent ink (highlighter and fades composite as
  one layer), translucent filled cone.
- **Tools:** pen, line, arrow, double arrow, curved arrow (follows the drag
  path, bows either direction), rectangle, ellipse, cone, spotlight,
  horizontal guide line, vertical guide line, eraser.
- **Styles:** dashed, filled, highlighter, opacity control, laser (fading
  ink), auto-fade timer.

### Input
- **Telestrator Draw dock**: a live program view with native Qt input; ink
  lands pixel-exact under the cursor (HiDPI and letterbox aware). Right-click
  for Fit to Window or Fill Window. This is the canonical drawing surface: no
  projector, no global cursor polling.
- Legacy Win32 projector / main-preview cursor input kept behind an opt-in
  settings toggle.

### Control surface
- **Three native docks** (Tools / Color / Replay) in OBS's own theme: checkable
  tool palette, style toggles, brush-size slider, OBS's Select Color grid with
  an active-swatch ring, undo / redo / clear, the replay markup flow, and
  one-click **Add to Current Scene / Remove from Scene** management. Dock state
  syncs from the engine, so hotkeys and Stream Deck stay in lockstep. One-time
  canonical dock placement on first load.
- **Hotkeys:** the full `telestrator.*` vocabulary (tools, colors, sizes,
  styles, arm, undo/redo/clear, replay transport, projector management).
- **obs-websocket vendor requests:** `set_color` (arbitrary RGB ink) and
  `sim_stroke` (scripted strokes through the canonical input path, used by the
  automated demo and integration tests).

### Replay
- **Markup replay:** save the replay buffer and draw over the clip
  (play / pause / stop / restart), then resume live. No scene switch.

[1.0.0]: https://github.com/brendanwelsh/obs-telestrator/releases/tag/v1.0.0
